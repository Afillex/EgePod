/* Simulation ALSA backend — compiled only when USE_ALSA_LIB=1.
 *
 * Audio output priority:
 *   1. EGEPOD_AUDIO_OUT=tcp://host:port  TCP socket to macOS ffplay listener.
 *   2. EGEPOD_AUDIO_OUT=<path>           raw s16le to file or FIFO.
 *   3. System libasound (if VM has a real soundcard).
 *   4. Default file fallback: /tmp/egepod_audio.raw
 *
 * TCP path — Singleton Bridge Architecture
 * ─────────────────────────────────────────
 * One persistent TCP connection is opened on first use and kept alive for the
 * entire process lifetime.  The same AlsaOut pointer is returned on every
 * alsa_out_open call, and alsa_out_close does NOT close the socket — it only
 * resets per-track resampler state and clears the "active" flag.
 *
 * Invariants guaranteed by construction:
 *   • Track changes, stop/play transitions, and pause/resume never touch the
 *     network.  No reconnect delay between tracks.
 *   • Output on the wire is always s16le / 44100 Hz / stereo.  ffplay's -ar and
 *     -ch_layout flags never need to change.
 *   • The AlsaOut pointer returned to player.c is never freed.  player.c's
 *     capture-then-write race (load_track closes AlsaOut while playback thread
 *     is mid-write) is therefore harmless — the pointer always references live
 *     static storage.
 *   • One silence thread, started once, writes zeros into ffplay's queue
 *     whenever active==0 (between tracks) or paused==1.  ffplay's decoder never
 *     starves and never disconnects.
 *   • All writes (real audio and silence) share one mutex — no interleaved bytes
 *     on the TCP stream.
 */

#ifdef USE_ALSA_LIB

#include "alsa_out.h"
#include "../common/log.h"

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define AUDIO_OUT_DEFAULT   "/tmp/egepod_audio.raw"
#define OUT_RATE_FIXED      44100u   /* wire sample rate; ffplay is always at this */
#define OUT_CH_FIXED        2u       /* wire channels; always stereo */
#define RESAMP_BUF_FRAMES   16384u   /* scratch for resampled output */
#define SILENCE_FRAMES      1024u    /* ~23 ms per silence chunk */

/* ══════════════════════════════════════════════════════════════════════════
 * Singleton TCP bridge state
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* network */
    int             fd;             /* -1 = disconnected */
    char            host[128];
    int             port;
    pthread_mutex_t wlock;          /* every write to fd goes through this */

    /* per-track resampler state — reset on every alsa_out_open */
    unsigned        in_rate;        /* decoder's native sample rate */
    unsigned        in_ch;          /* decoder's channel count (1 or 2) */
    int64_t         resamp_acc;     /* signed Q32.32 phase accumulator */
    int16_t        *resamp_buf;     /* RESAMP_BUF_FRAMES × OUT_CH_FIXED int16 */
    int16_t         resamp_prev[OUT_CH_FIXED]; /* last input frame, for chunk-boundary interp */
    int             resamp_prev_valid;
    struct timespec deadline;
    int             deadline_set;

    /* sink state flags */
    volatile int    active;         /* 1 while a track is opened (between open & close) */
    volatile int    paused;         /* 1 between alsa_out_pause and alsa_out_resume */

    /* silence thread (process-lifetime; started lazily on first TCP open) */
    pthread_t       silence_tid;
    volatile int    silence_run;
} TcpSink;

static TcpSink  g_tcp;
static int      g_tcp_inited;       /* set to 1 after first successful TCP open */

/* ══════════════════════════════════════════════════════════════════════════
 * AlsaOut struct
 * ══════════════════════════════════════════════════════════════════════════ */

struct AlsaOut {
    int is_tcp;          /* 1 = this is the TCP singleton, all other fields unused */

    /* Non-TCP path fields */
    snd_pcm_t    *pcm;
    FILE         *raw_file;
    int           is_fifo;
    unsigned int  channels;
    unsigned int  rate;
    struct timespec deadline;
    int             deadline_set;
};

/* Returned to player.c as the "AlsaOut *" on the TCP path.
 * Never freed — player.c's pointer is always valid. */
static AlsaOut  g_tcp_handle;

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

/* Open a file or FIFO for writing. */
static FILE *open_raw_out(const char *path, int *is_fifo_out)
{
    struct stat st;
    *is_fifo_out = 0;

    if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode)) {
        *is_fifo_out = 1;
        int fd = -1;
        for (int try = 0; try < 20; try++) {
            fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) break;
            if (errno != ENXIO) break;
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
        if (fd < 0) {
            LOGE("alsa_sim: cannot open FIFO %s: %s", path, strerror(errno));
            return NULL;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        FILE *f = fdopen(fd, "wb");
        if (!f) { close(fd); return NULL; }
        LOGI("alsa_sim: writing raw s16le PCM to FIFO %s", path);
        return f;
    }

    FILE *f = fopen(path, "wb");
    if (!f) LOGE("alsa_sim: cannot open %s: %s", path, strerror(errno));
    else    LOGI("alsa_sim: writing raw s16le PCM to file %s", path);
    return f;
}

/* Connect to macOS host over TCP; retries up to ~5 s.
 * Returns a connected socket fd, or -1 on failure. */
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

                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    /* Bound send buffer: ~150 ms at 44100 Hz stereo s16le ≈ 26 kB.
                     * Kernels may double internally; that's fine.  The bound ensures
                     * pause takes effect quickly instead of draining a huge buffer. */
                    int sndbuf = 32 * 1024;
                    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                    LOGI("alsa_sim: TCP connected to %s:%d", host, port);
                    return fd;
                }
                close(fd);
            }
            freeaddrinfo(res);
        }
        if (attempt < 24) {
            struct timespec ts = {0, 200000000};
            nanosleep(&ts, NULL);
        }
    }
    LOGE("alsa_sim: TCP connect to %s:%d timed out", host, port);
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Resampler + stereo upmix (TCP path)
 *
 * Converts g_tcp.in_ch / g_tcp.in_rate input to OUT_CH_FIXED / OUT_RATE_FIXED.
 *
 * Linear interpolation resample.  Phase state g_tcp.resamp_acc is signed Q32.32:
 * a negative value at chunk-start is resolved via g_tcp.resamp_prev (last frame
 * of the previous chunk).  Mono input is duplicated to stereo.
 *
 * Must be called under g_tcp.wlock so it doesn't race with alsa_out_open's
 * per-track state reset.
 *
 * Returns number of output frames written to obuf.
 * ══════════════════════════════════════════════════════════════════════════ */
static size_t resamp_and_mix(const int16_t *in, size_t in_frames,
                              int16_t *obuf, size_t obuf_max)
{
    const unsigned in_ch = g_tcp.in_ch;

    /* ── fast paths ──────────────────────────────────────────────────────── */

    if (in_ch == 2 && g_tcp.in_rate == OUT_RATE_FIXED) {
        size_t n = in_frames < obuf_max ? in_frames : obuf_max;
        memcpy(obuf, in, n * 2 * sizeof(int16_t));
        if (n > 0) {
            g_tcp.resamp_prev[0] = in[(n-1)*2+0];
            g_tcp.resamp_prev[1] = in[(n-1)*2+1];
            g_tcp.resamp_prev_valid = 1;
        }
        g_tcp.resamp_acc = 0;
        return n;
    }

    if (in_ch == 1 && g_tcp.in_rate == OUT_RATE_FIXED) {
        size_t n = in_frames < obuf_max ? in_frames : obuf_max;
        for (size_t i = 0; i < n; i++) { obuf[i*2] = in[i]; obuf[i*2+1] = in[i]; }
        if (n > 0) { g_tcp.resamp_prev[0] = g_tcp.resamp_prev[1] = in[n-1]; }
        g_tcp.resamp_prev_valid = (n > 0);
        g_tcp.resamp_acc = 0;
        return n;
    }

    /* ── resample with optional mono→stereo upmix ────────────────────────── */

    int64_t  step  = ((int64_t)g_tcp.in_rate << 32) / (int64_t)OUT_RATE_FIXED;
    int64_t  phase = g_tcp.resamp_acc;
    size_t   o     = 0;

    while (o < obuf_max) {
        int64_t  ip   = phase >> 32;
        uint32_t frac = (uint32_t)(phase & (int64_t)0xFFFFFFFF);

        /* Need ip and ip+1 in the current chunk */
        if (ip + 1 >= (int64_t)in_frames)
            break;

        if (in_ch == 2) {
            for (int c = 0; c < 2; c++) {
                int32_t a = (ip < 0) ? (g_tcp.resamp_prev_valid ? g_tcp.resamp_prev[c] : 0)
                                     : in[(size_t)ip * 2 + c];
                int32_t b = in[(size_t)(ip+1) * 2 + c];
                obuf[o*2+c] = (int16_t)(a + (int32_t)(((int64_t)(b-a) * frac) >> 32));
            }
        } else {
            int32_t a = (ip < 0) ? (g_tcp.resamp_prev_valid ? g_tcp.resamp_prev[0] : 0)
                                 : (int32_t)in[(size_t)ip];
            int32_t b = (int32_t)in[(size_t)(ip+1)];
            int16_t s = (int16_t)(a + (int32_t)(((int64_t)(b-a) * frac) >> 32));
            obuf[o*2+0] = s; obuf[o*2+1] = s;
        }
        o++;
        phase += step;
    }

    if (in_frames > 0) {
        if (in_ch == 2) {
            g_tcp.resamp_prev[0] = in[(in_frames-1)*2+0];
            g_tcp.resamp_prev[1] = in[(in_frames-1)*2+1];
        } else {
            g_tcp.resamp_prev[0] = g_tcp.resamp_prev[1] = in[in_frames-1];
        }
        g_tcp.resamp_prev_valid = 1;
    }
    g_tcp.resamp_acc = phase - ((int64_t)in_frames << 32);
    return o;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Silence thread — process-lifetime
 *
 * Writes zeroed PCM to the TCP socket at OUT_RATE_FIXED / OUT_CH_FIXED
 * pacing whenever no real audio is flowing (active==0 or paused==1).
 * Keeps ffplay's audio decoder alive so it never stalls or disconnects.
 * ══════════════════════════════════════════════════════════════════════════ */
static void *silence_thread_fn(void *arg)
{
    (void)arg;
    int16_t zeros[SILENCE_FRAMES * OUT_CH_FIXED];
    memset(zeros, 0, sizeof(zeros));

    /* Nanoseconds per silence chunk — used for absolute-time pacing. */
    const uint64_t ns = (uint64_t)SILENCE_FRAMES * 1000000000ULL / OUT_RATE_FIXED;

    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);

    while (g_tcp.silence_run) {
        /* Advance absolute deadline by one chunk */
        dl.tv_nsec += (long)(ns % 1000000000ULL);
        dl.tv_sec  += (long)(ns / 1000000000ULL);
        if (dl.tv_nsec >= 1000000000L) { dl.tv_nsec -= 1000000000L; dl.tv_sec++; }

        pthread_mutex_lock(&g_tcp.wlock);
        if ((!g_tcp.active || g_tcp.paused) && g_tcp.fd >= 0) {
            const uint8_t *buf = (const uint8_t *)zeros;
            size_t nb = sizeof(zeros);
            while (nb > 0) {
                ssize_t n = write(g_tcp.fd, buf, nb);
                if (n <= 0) { close(g_tcp.fd); g_tcp.fd = -1; break; }
                buf += (size_t)n; nb -= (size_t)n;
            }
        }
        pthread_mutex_unlock(&g_tcp.wlock);

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL);
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API — TCP path (singleton)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Ensure TCP is connected (fast path when already open).
 * Called with wlock held only for the brief fd assignment; actual connect
 * happens outside the lock so the silence thread is not blocked. */
static int tcp_ensure_connected(void)
{
    if (g_tcp.fd >= 0) return 0;
    /* Unlock briefly so the silence thread is not starved during reconnect */
    pthread_mutex_unlock(&g_tcp.wlock);
    LOGI("alsa_sim: TCP disconnected — reconnecting to %s:%d ...", g_tcp.host, g_tcp.port);
    int fd = open_tcp_out(g_tcp.host, g_tcp.port);
    pthread_mutex_lock(&g_tcp.wlock);
    if (g_tcp.fd < 0) g_tcp.fd = fd;   /* only assign if nobody else reconnected */
    else if (fd >= 0) close(fd);
    return g_tcp.fd >= 0 ? 0 : -1;
}

/* ── alsa_out_open ───────────────────────────────────────────────────────── */

AlsaOut *alsa_out_open(unsigned int card, unsigned int device,
                       unsigned int rate, unsigned int channels,
                       unsigned int bits)
{
    (void)card; (void)device; (void)bits;

    const char *env_path = getenv("EGEPOD_AUDIO_OUT");

    /* ── TCP singleton path ─────────────────────────────────────────────── */
    if (env_path && strncmp(env_path, "tcp://", 6) == 0) {

        if (!g_tcp_inited) {
            /* One-time setup */
            pthread_mutex_init(&g_tcp.wlock, NULL);

            /* Parse host:port from env */
            const char *rest  = env_path + 6;
            const char *colon = strrchr(rest, ':');
            if (colon) {
                size_t hl = (size_t)(colon - rest);
                if (hl < sizeof(g_tcp.host)) {
                    memcpy(g_tcp.host, rest, hl);
                    g_tcp.host[hl] = '\0';
                }
                g_tcp.port = atoi(colon + 1);
            } else {
                snprintf(g_tcp.host, sizeof(g_tcp.host), "%s", rest);
                g_tcp.port = 12321;
            }

            g_tcp.resamp_buf = malloc(RESAMP_BUF_FRAMES * OUT_CH_FIXED * sizeof(int16_t));
            if (!g_tcp.resamp_buf) return NULL;

            g_tcp.fd = -1;
            g_tcp.fd = open_tcp_out(g_tcp.host, g_tcp.port);
            if (g_tcp.fd < 0) { free(g_tcp.resamp_buf); return NULL; }

            /* Start the silence thread once */
            g_tcp.silence_run = 1;
            pthread_create(&g_tcp.silence_tid, NULL, silence_thread_fn, NULL);

            g_tcp_handle.is_tcp = 1;
            g_tcp_inited        = 1;
        }

        /* Per-track state reset (runs on every open, including track changes) */
        pthread_mutex_lock(&g_tcp.wlock);
        g_tcp.in_rate           = rate;
        g_tcp.in_ch             = channels < 1 ? 1 : (channels > 2 ? 2 : channels);
        g_tcp.resamp_acc        = 0;
        g_tcp.resamp_prev[0]    = 0;
        g_tcp.resamp_prev[1]    = 0;
        g_tcp.resamp_prev_valid = 0;
        g_tcp.deadline_set      = 0;
        g_tcp.active            = 1;
        g_tcp.paused            = 0;
        pthread_mutex_unlock(&g_tcp.wlock);

        LOGI("alsa_sim: TCP sink: %u Hz / %u ch → %u Hz / %u ch  fd=%d",
             rate, channels, OUT_RATE_FIXED, OUT_CH_FIXED, g_tcp.fd);
        return &g_tcp_handle;
    }

    /* ── Non-TCP paths (unchanged behaviour) ─────────────────────────────── */

    AlsaOut *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->is_tcp   = 0;
    out->channels = channels;
    out->rate     = rate;

    if (!env_path) {
        out->pcm = try_alsa_open(rate, channels);
        if (out->pcm) return out;
        env_path = AUDIO_OUT_DEFAULT;
    }

    out->raw_file = open_raw_out(env_path, &out->is_fifo);
    if (!out->raw_file) { free(out); return NULL; }
    LOGI("alsa_sim: audio out: %u Hz / %u ch  [%s]",
         rate, channels, out->is_fifo ? "FIFO" : "file");
    return out;
}

/* ── alsa_out_write ──────────────────────────────────────────────────────── */

int alsa_out_write(AlsaOut *out, const int16_t *frames, size_t frame_count)
{
    /* ── TCP singleton path ─────────────────────────────────────────────── */
    if (out->is_tcp) {
        /* Resample + upmix under the mutex so in_rate/in_ch can't change mid-resample.
         * Also computes pacing deadline and performs the TCP write in one critical
         * section; sleep happens after unlock so the silence thread is not blocked. */
        struct timespec sleep_until;
        int write_err = 0;

        pthread_mutex_lock(&g_tcp.wlock);

        /* Resample and upmix */
        size_t out_frames = resamp_and_mix(frames, frame_count,
                                           g_tcp.resamp_buf, RESAMP_BUF_FRAMES);

        if (out_frames == 0) {
            pthread_mutex_unlock(&g_tcp.wlock);
            return 0;
        }

        /* Pacing: advance absolute deadline by this chunk's duration.
         * Self-correcting: if deadline has fallen behind real time, reset to now. */
        uint64_t ns = (uint64_t)out_frames * 1000000000ULL / OUT_RATE_FIXED;
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_ns = (int64_t)now.tv_sec  * 1000000000LL + now.tv_nsec;
            int64_t dl_ns  = (int64_t)g_tcp.deadline.tv_sec * 1000000000LL
                           + g_tcp.deadline.tv_nsec;
            if (!g_tcp.deadline_set || dl_ns + (int64_t)ns < now_ns) {
                g_tcp.deadline     = now;
                g_tcp.deadline_set = 1;
            }
        }
        g_tcp.deadline.tv_nsec += (long)(ns % 1000000000ULL);
        g_tcp.deadline.tv_sec  += (long)(ns / 1000000000ULL);
        if (g_tcp.deadline.tv_nsec >= 1000000000L) {
            g_tcp.deadline.tv_nsec -= 1000000000L;
            g_tcp.deadline.tv_sec++;
        }
        sleep_until = g_tcp.deadline;

        /* Reconnect if needed (drops and re-acquires wlock during blocking op) */
        if (g_tcp.fd < 0)
            write_err = (tcp_ensure_connected() != 0);

        /* Write */
        if (!write_err && g_tcp.fd >= 0) {
            const uint8_t *buf = (const uint8_t *)g_tcp.resamp_buf;
            size_t nb = out_frames * OUT_CH_FIXED * sizeof(int16_t);
            while (nb > 0) {
                ssize_t n = write(g_tcp.fd, buf, nb);
                if (n <= 0) {
                    LOGE("alsa_sim: TCP write: %s", strerror(errno));
                    close(g_tcp.fd); g_tcp.fd = -1; write_err = 1; break;
                }
                buf += (size_t)n; nb -= (size_t)n;
            }
        } else if (g_tcp.fd < 0) {
            write_err = 1;
        }

        pthread_mutex_unlock(&g_tcp.wlock);

        /* Pace after releasing the lock */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_until, NULL);
        return write_err ? -1 : 0;
    }

    /* ── real ALSA path ──────────────────────────────────────────────────── */
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

    /* ── file / FIFO path ────────────────────────────────────────────────── */
    if (out->raw_file) {
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
        out->deadline.tv_nsec += (long)(ns_per_period % 1000000000ULL);
        out->deadline.tv_sec  += (long)(ns_per_period / 1000000000ULL);
        if (out->deadline.tv_nsec >= 1000000000L) {
            out->deadline.tv_nsec -= 1000000000L;
            out->deadline.tv_sec++;
        }

        size_t n = fwrite(frames, sizeof(int16_t), frame_count * out->channels, out->raw_file);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &out->deadline, NULL);
        return (n == frame_count * out->channels) ? 0 : -1;
    }

    /* No sink: spin-sleep to prevent a hot loop */
    {
        uint64_t ns = (uint64_t)frame_count * 1000000000ULL / out->rate;
        struct timespec ts = { .tv_sec  = (time_t)(ns / 1000000000ULL),
                               .tv_nsec = (long)(ns % 1000000000ULL) };
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ── alsa_out_pause ──────────────────────────────────────────────────────── */

int alsa_out_pause(AlsaOut *out)
{
    if (out->is_tcp) {
        pthread_mutex_lock(&g_tcp.wlock);
        g_tcp.paused       = 1;
        g_tcp.deadline_set = 0;  /* next real write will re-anchor pacing */
        pthread_mutex_unlock(&g_tcp.wlock);
        return 0;
    }
    if (!out->pcm) return 0;
    snd_pcm_drop(out->pcm);
    snd_pcm_prepare(out->pcm);
    return 0;
}

/* ── alsa_out_resume ─────────────────────────────────────────────────────── */

int alsa_out_resume(AlsaOut *out)
{
    if (out->is_tcp) {
        pthread_mutex_lock(&g_tcp.wlock);
        g_tcp.paused       = 0;
        g_tcp.deadline_set = 0;  /* re-anchor pacing from current time */
        pthread_mutex_unlock(&g_tcp.wlock);
        return 0;
    }
    return 0;
}

/* ── alsa_out_matches_format ─────────────────────────────────────────────── */

int alsa_out_matches_format(const AlsaOut *out, unsigned int rate, unsigned int channels)
{
    if (!out) return 0;
    if (out->is_tcp) {
        /* Return 0 whenever rate or channels differ so player.c calls close+open,
         * which triggers the per-track resampler reset in alsa_out_open.
         * Return 1 only when formats match exactly to skip the redundant reset. */
        return (g_tcp.in_rate == rate && g_tcp.in_ch == channels);
    }
    return (out->rate == rate && out->channels == channels);
}

/* ── alsa_out_close ──────────────────────────────────────────────────────── */

void alsa_out_close(AlsaOut *out)
{
    if (!out) return;

    if (out->is_tcp) {
        /* TCP: do NOT close the socket or stop the silence thread.
         * Just clear "active" so the silence thread fills the gap until the
         * next alsa_out_open.  The pointer itself is never freed. */
        pthread_mutex_lock(&g_tcp.wlock);
        g_tcp.active       = 0;
        g_tcp.deadline_set = 0;
        pthread_mutex_unlock(&g_tcp.wlock);
        return;
    }

    if (out->pcm)      { snd_pcm_drain(out->pcm); snd_pcm_close(out->pcm); }
    if (out->raw_file) fclose(out->raw_file);
    free(out);
}

#endif /* USE_ALSA_LIB */
