/* Simulation ALSA backend — compiled only when USE_ALSA_LIB=1.
 *
 * Audio output priority:
 *   1. EGEPOD_AUDIO_OUT=tcp://host:port → TCP socket to macOS ffplay listener.
 *      simulate.sh sets this to tcp://host.orb.internal:12321 so audiod streams
 *      raw s16le PCM directly to macOS without going through the orb bridge or
 *      the VM's PulseAudio null sink.
 *   2. EGEPOD_AUDIO_OUT=<path> → write raw s16le to a file or FIFO.
 *   3. System libasound (works if the VM has a real soundcard).
 *   4. Default file fallback: /tmp/egepod_audio.raw
 *
 * TCP pause/resume: alsa_out_pause starts a silence-fill thread that sends
 * zeroed PCM at the correct rate, keeping ffplay's CoreAudio queue alive and
 * preventing queue starvation.  alsa_out_resume stops it.  tcp_lock serialises
 * real writes against the silence thread so the stream is never interleaved.
 */

#ifdef USE_ALSA_LIB

#include "alsa_out.h"
#include "../common/log.h"

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>

#define AUDIO_OUT_DEFAULT "/tmp/egepod_audio.raw"
#define SILENCE_PERIOD_FRAMES 4096u

struct AlsaOut {
    snd_pcm_t    *pcm;      /* non-NULL → real ALSA device */
    FILE         *raw_file; /* non-NULL → file/pipe fallback */
    int           is_fifo;  /* 1 → raw_file is a FIFO, skip nanosleep pacing */
    int           tcp_fd;   /* >= 0 → TCP stream to macOS host */
    unsigned int  channels;
    unsigned int  rate;
    /* Persistent pacing deadline — advanced by exactly one chunk duration
     * per write so that OS scheduling jitter is self-correcting rather
     * than accumulating.  Initialized on the first write of each session. */
    struct timespec deadline;
    int             deadline_set;

    /* TCP path: silence-fill during pause keeps ffplay's CoreAudio queue alive.
     * tcp_lock serialises all writes to tcp_fd (real and silence threads).     */
    pthread_mutex_t  tcp_lock;
    pthread_t        silence_tid;
    volatile int     silence_run;     /* 1 → silence thread should fill */
    int              silence_started; /* 1 → pthread was created, must be joined */
};

/* ── real ALSA open ──────────────────────────────────────────────────────── */

static snd_pcm_t *try_alsa_open(unsigned int rate, unsigned int channels)
{
    const char *names[] = { "hw:0,0", "plughw:0,0", "default", "pulse", NULL };
    for (int i = 0; names[i]; i++) {
        snd_pcm_t *pcm = NULL;
        int rc = snd_pcm_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (rc < 0) continue;

        snd_pcm_hw_params_t *p;
        snd_pcm_hw_params_alloca(&p);
        snd_pcm_hw_params_any(pcm, p);
        snd_pcm_hw_params_set_access(pcm, p, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, p, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm, p, channels);
        snd_pcm_hw_params_set_rate_near(pcm, p, &rate, 0);
        snd_pcm_uframes_t period = 4096;
        unsigned int periods = 4;
        snd_pcm_hw_params_set_period_size_near(pcm, p, &period, 0);
        snd_pcm_hw_params_set_periods_near(pcm, p, &periods, 0);
        rc = snd_pcm_hw_params(pcm, p);
        if (rc < 0) { snd_pcm_close(pcm); continue; }

        LOGI("alsa_sim: opened PCM '%s' @ %u Hz / %u ch", names[i], rate, channels);
        return pcm;
    }
    return NULL;
}

/* Open a file or FIFO for writing.  For FIFOs, open non-blocking first (to
 * avoid a deadlock if the reader is not ready yet), then retry up to ~2s,
 * then switch back to blocking writes so fwrite() paces naturally. */
static FILE *open_raw_out(const char *path, int *is_fifo_out)
{
    struct stat st;
    *is_fifo_out = 0;

    /* Probe without opening */
    if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode)) {
        *is_fifo_out = 1;
        /* Open FIFO non-blocking so we don't deadlock if reader is slow */
        int fd = -1;
        for (int try = 0; try < 20; try++) {
            fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) break;
            if (errno != ENXIO) break;   /* reader not attached yet → retry */
            struct timespec ts = {0, 100000000}; /* 100 ms */
            nanosleep(&ts, NULL);
        }
        if (fd < 0) {
            LOGE("alsa_sim: cannot open FIFO %s: %s", path, strerror(errno));
            return NULL;
        }
        /* Switch to blocking — fwrite() will now pace to consumer speed */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        FILE *f = fdopen(fd, "wb");
        if (!f) { close(fd); return NULL; }
        LOGI("alsa_sim: writing raw s16le PCM to FIFO %s", path);
        return f;
    }

    /* Regular file */
    FILE *f = fopen(path, "wb");
    if (!f) LOGE("alsa_sim: cannot open %s: %s", path, strerror(errno));
    else    LOGI("alsa_sim: writing raw s16le PCM to file %s", path);
    return f;
}

/* Connect to macOS host over TCP; retries for ~5 s so audiod can start before
 * ffplay is ready.  Returns a connected socket fd, or -1 on failure. */
static int open_tcp_out(const char *host, int port)
{
    char svc[16];
    snprintf(svc, sizeof(svc), "%d", port);
    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    for (int attempt = 0; attempt < 25; attempt++) {
        struct addrinfo *res = NULL;
        if (getaddrinfo(host, svc, &hints, &res) == 0) {
            int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (fd >= 0) {
                if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
                    freeaddrinfo(res);
                    LOGI("alsa_sim: TCP connected to %s:%d", host, port);
                    return fd;
                }
                close(fd);
            }
            freeaddrinfo(res);
        }
        if (attempt < 24) {
            struct timespec ts = {0, 200000000}; /* 200 ms */
            nanosleep(&ts, NULL);
        }
    }
    LOGE("alsa_sim: TCP connect to %s:%d timed out", host, port);
    return -1;
}

/* ── silence-fill thread (TCP path only) ─────────────────────────────────── */
/*
 * Sends zeroed PCM frames at the same rate as real playback while the player
 * is paused.  This keeps ffplay's CoreAudio output queue alive so audio
 * resumes seamlessly when real data returns.
 *
 * Serialisation: tcp_lock is held only during the write() call.  The sleep
 * happens outside the lock so real-audio writes can interleave at the chunk
 * boundary.  out->deadline is protected by tcp_lock.
 */
static void *silence_thread(void *arg)
{
    AlsaOut *out = arg;
    const size_t period = SILENCE_PERIOD_FRAMES;
    size_t nbytes = period * out->channels * sizeof(int16_t);
    int16_t *buf = calloc(period * out->channels, sizeof(int16_t));
    if (!buf) return NULL;

    LOGI("alsa_sim: silence thread started (fd=%d %u Hz %u ch)",
         out->tcp_fd, out->rate, out->channels);

    while (out->silence_run && out->tcp_fd >= 0) {
        uint64_t ns = (uint64_t)period * 1000000000ULL / out->rate;
        struct timespec sleep_until;

        pthread_mutex_lock(&out->tcp_lock);

        if (!out->silence_run) {           /* recheck under lock */
            pthread_mutex_unlock(&out->tcp_lock);
            break;
        }

        /* Advance the persistent deadline by one chunk duration. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_ns = (int64_t)now.tv_sec  * 1000000000LL + now.tv_nsec;
            int64_t dl_ns  = (int64_t)out->deadline.tv_sec * 1000000000LL
                           + out->deadline.tv_nsec;
            if (!out->deadline_set || dl_ns + (int64_t)ns < now_ns) {
                out->deadline     = now;
                out->deadline_set = 1;
            }
        }
        out->deadline.tv_nsec += (long)(ns % 1000000000ULL);
        out->deadline.tv_sec  += (long)(ns / 1000000000ULL);
        if (out->deadline.tv_nsec >= 1000000000L) {
            out->deadline.tv_nsec -= 1000000000L;
            out->deadline.tv_sec++;
        }
        sleep_until = out->deadline;

        /* Write silence */
        const uint8_t *p = (const uint8_t *)buf;
        size_t rem = nbytes;
        int write_ok = 1;
        while (rem > 0) {
            ssize_t n = write(out->tcp_fd, p, rem);
            if (n <= 0) {
                LOGE("alsa_sim: silence TCP write: %s", strerror(errno));
                write_ok = 0;
                break;
            }
            p   += (size_t)n;
            rem -= (size_t)n;
        }

        pthread_mutex_unlock(&out->tcp_lock);

        if (!write_ok) break;

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_until, NULL);
    }

    LOGI("alsa_sim: silence thread exiting");
    free(buf);
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────────── */

AlsaOut *alsa_out_open(unsigned int card, unsigned int device,
                       unsigned int rate, unsigned int channels,
                       unsigned int bits)
{
    (void)card; (void)device; (void)bits;

    AlsaOut *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->channels = channels;
    out->rate     = rate;
    out->tcp_fd   = -1;
    pthread_mutex_init(&out->tcp_lock, NULL);

    /* If an explicit output path is requested, bypass ALSA/PulseAudio entirely.
     * Writing directly to a FIFO is far more stable in simulation than going
     * through PulseAudio's pipe-sink (no buffering storms, no EIO on restart). */
    const char *env_path = getenv("EGEPOD_AUDIO_OUT");
    if (!env_path) {
        /* No explicit path — try ALSA first */
        out->pcm = try_alsa_open(rate, channels);
        if (out->pcm) return out;
        env_path = AUDIO_OUT_DEFAULT;
    }

    /* tcp://host:port — direct TCP stream; crosses VM→macOS without orb overhead */
    if (strncmp(env_path, "tcp://", 6) == 0) {
        char host[128] = "127.0.0.1";
        int  port      = 12321;
        const char *rest  = env_path + 6;
        const char *colon = strrchr(rest, ':');
        if (colon) {
            size_t hl = (size_t)(colon - rest);
            if (hl < sizeof(host)) { memcpy(host, rest, hl); host[hl] = '\0'; }
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", rest);
        }
        out->tcp_fd = open_tcp_out(host, port);
        if (out->tcp_fd < 0) { free(out); return NULL; }
        LOGI("alsa_sim: audio out: %u Hz / %u ch  [TCP → %s:%d]",
             rate, channels, host, port);
        return out;
    }

    out->raw_file = open_raw_out(env_path, &out->is_fifo);
    if (!out->raw_file) {
        free(out);
        return NULL;
    }
    LOGI("alsa_sim: audio out: %u Hz / %u ch  [%s]",
         rate, channels, out->is_fifo ? "FIFO — natural pacing" : "file — nanosleep pacing");
    return out;
}

int alsa_out_write(AlsaOut *out, const int16_t *frames, size_t frame_count)
{
    if (out->tcp_fd >= 0) {
        uint64_t ns = (uint64_t)frame_count * 1000000000ULL / out->rate;
        struct timespec sleep_until;

        /* tcp_lock serialises against the silence-fill thread. */
        pthread_mutex_lock(&out->tcp_lock);

        /* Advance the persistent deadline by exactly one chunk duration.
         * Using a running absolute deadline means late OS wakeups are
         * automatically compensated in the next sleep — jitter never
         * accumulates into long-term clock drift. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_ns = (int64_t)now.tv_sec  * 1000000000LL + now.tv_nsec;
            int64_t dl_ns  = (int64_t)out->deadline.tv_sec * 1000000000LL
                           + out->deadline.tv_nsec;
            /* Reset if not yet initialised, or if the stored deadline is more
             * than one chunk in the past (e.g. after a pause). */
            if (!out->deadline_set || dl_ns + (int64_t)ns < now_ns) {
                out->deadline     = now;
                out->deadline_set = 1;
            }
        }
        out->deadline.tv_nsec += (long)(ns % 1000000000ULL);
        out->deadline.tv_sec  += (long)(ns / 1000000000ULL);
        if (out->deadline.tv_nsec >= 1000000000L) {
            out->deadline.tv_nsec -= 1000000000L;
            out->deadline.tv_sec++;
        }
        sleep_until = out->deadline;

        const uint8_t *buf    = (const uint8_t *)frames;
        size_t         nbytes = frame_count * out->channels * sizeof(int16_t);
        while (nbytes > 0) {
            ssize_t n = write(out->tcp_fd, buf, nbytes);
            if (n <= 0) {
                LOGE("alsa_sim: TCP write: %s", strerror(errno));
                pthread_mutex_unlock(&out->tcp_lock);
                return -1;
            }
            buf    += (size_t)n;
            nbytes -= (size_t)n;
        }

        pthread_mutex_unlock(&out->tcp_lock);

        /* Sleep outside the lock so silence thread can interleave at boundaries. */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_until, NULL);
        return 0;
    }

    if (out->pcm) {
        snd_pcm_sframes_t r = snd_pcm_writei(out->pcm, frames, frame_count);
        if (r < 0) {
            LOGW("alsa_sim: writei error: %s", snd_strerror((int)r));
            int rc = snd_pcm_recover(out->pcm, (int)r, 1);
            if (rc < 0) {
                LOGE("alsa_sim: recover failed: %s", snd_strerror(rc));
                return -1;
            }
            r = snd_pcm_writei(out->pcm, frames, frame_count);
            if (r < 0)
                LOGE("alsa_sim: retry writei error: %s", snd_strerror((int)r));
        }
        return (r < 0) ? -1 : 0;
    }

    if (out->raw_file) {
        /* Advance persistent deadline by exactly one chunk duration — same
         * self-correcting logic as the TCP path above. */
        uint64_t ns_per_period = (uint64_t)frame_count * 1000000000ULL / out->rate;
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_ns = (int64_t)now.tv_sec  * 1000000000LL + now.tv_nsec;
            int64_t dl_ns  = (int64_t)out->deadline.tv_sec * 1000000000LL
                           + out->deadline.tv_nsec;
            if (!out->deadline_set || dl_ns + (int64_t)ns_per_period < now_ns) {
                out->deadline     = now;
                out->deadline_set = 1;
            }
        }
        struct timespec t0 = out->deadline;  /* for diagnostics */
        out->deadline.tv_nsec += (long)(ns_per_period % 1000000000ULL);
        out->deadline.tv_sec  += (long)(ns_per_period / 1000000000ULL);
        if (out->deadline.tv_nsec >= 1000000000L) {
            out->deadline.tv_nsec -= 1000000000L;
            out->deadline.tv_sec++;
        }

        size_t n = fwrite(frames, sizeof(int16_t), frame_count * out->channels,
                          out->raw_file);
        /* Sleep until deadline — zero-cost if fwrite already took enough time. */
        int snret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &out->deadline, NULL);

        /* Diagnostic: log every 50 writes and flag any early wakeup */
        {
            static uint64_t _wn = 0;
            static struct timespec _batch_t0;
            if (_wn == 0) _batch_t0 = t0;
            _wn++;
            if (snret != 0)
                LOGW("alsa_sim: w#%lu clock_nanosleep early (ret=%d) rate=%u fc=%zu",
                     (unsigned long)_wn, snret, out->rate, frame_count);
            if ((_wn % 50) == 0) {
                struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
                long ms = (tn.tv_sec  - _batch_t0.tv_sec)  * 1000
                        + (tn.tv_nsec - _batch_t0.tv_nsec) / 1000000;
                LOGI("alsa_sim: w#%lu: 50-write batch %ldms (exp ~4644ms) rate=%u",
                     (unsigned long)_wn, ms, out->rate);
                _batch_t0 = tn;
            }
        }

        return (n == frame_count * out->channels) ? 0 : -1;
    }

    /* No sink — spin-sleep to prevent a hot loop */
    {
        uint64_t ns = (uint64_t)frame_count * 1000000000ULL / out->rate;
        struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000ULL),
                               .tv_nsec = (long)(ns % 1000000000ULL) };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int alsa_out_pause(AlsaOut *out)
{
    if (out->tcp_fd >= 0) {
        /* Start silence-fill thread to keep ffplay's CoreAudio queue alive.
         * tcp_lock ensures the silence thread doesn't interleave with any
         * alsa_out_write that is still in flight. */
        if (!out->silence_started) {
            out->silence_run = 1;
            if (pthread_create(&out->silence_tid, NULL, silence_thread, out) == 0) {
                out->silence_started = 1;
                LOGI("alsa_sim: pause — silence thread started");
            } else {
                LOGE("alsa_sim: pause — silence thread create failed: %s",
                     strerror(errno));
                out->silence_run = 0;
            }
        }
        return 0;
    }

    if (!out->pcm) return 0;
    /* snd_pcm_pause is not supported by the PulseAudio ALSA plugin.
     * snd_pcm_drop immediately stops playback and flushes the ring buffer;
     * snd_pcm_prepare leaves the device ready for the next write on resume. */
    snd_pcm_drop(out->pcm);
    snd_pcm_prepare(out->pcm);
    return 0;
}

int alsa_out_resume(AlsaOut *out)
{
    if (out->tcp_fd >= 0) {
        /* Stop the silence thread and join it so the silence_started flag can
         * be reset.  This allows a subsequent pause to start a fresh thread.
         * pthread_join blocks at most one silence-chunk duration (~93 ms) since
         * the thread sleeps outside tcp_lock and exits when silence_run=0. */
        if (out->silence_started) {
            out->silence_run = 0;
            pthread_join(out->silence_tid, NULL);
            out->silence_started = 0;
            LOGI("alsa_sim: resume — silence thread joined");
        }
        return 0;
    }
    /* ALSA/file paths: first alsa_out_write will start it. */
    (void)out;
    return 0;
}

int alsa_out_matches_format(const AlsaOut *out, unsigned int rate, unsigned int channels)
{
    if (!out) return 0;
    return out->rate == rate && out->channels == channels;
}

void alsa_out_close(AlsaOut *out)
{
    if (!out) return;
    /* Stop and join the silence thread before closing tcp_fd. */
    if (out->silence_started) {
        out->silence_run = 0;
        pthread_join(out->silence_tid, NULL);
        out->silence_started = 0;
    }
    if (out->tcp_fd >= 0) close(out->tcp_fd);
    if (out->pcm)      { snd_pcm_drain(out->pcm); snd_pcm_close(out->pcm); }
    if (out->raw_file) fclose(out->raw_file);
    pthread_mutex_destroy(&out->tcp_lock);
    free(out);
}

#endif /* USE_ALSA_LIB */
