/* 🐧 Kernel & HAL Hacker + 🔋 Power Warden
 *
 * Player state machine with double-buffer pre-decode.
 *
 * Buffer lifecycle:
 *   cur  — track currently being played out (locked PCM in RAM)
 *   next — track being decoded in the background; swapped on track-end
 *
 * The eMMC is touched exactly once per track: during the loader thread's
 * slurp_file() call inside decoder_decode().  After that, playback runs
 * entirely from the mlock()'d PCM buffer. */

#include "player.h"
#include "decoder.h"
#include "alsa_out.h"
#include "index.h"
#include "../common/log.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sched.h>

#define ALSA_CARD      0
#define ALSA_DEVICE    0
#define MUSIC_DIR      "/sdcard/Music"
#define PERIOD_FRAMES  4096u   /* PCM frames per playback write (~93ms @ 44.1kHz) */

/* ── types ─────────────────────────────────────────────────────────────── */

typedef struct {
    int16_t *pcm;           /* locked PCM buffer (NULL = not loaded) */
    size_t   frames;
    uint32_t rate;
    uint32_t channels;
    TrackInfo info;
} PcmBuf;

struct Player {
    IndexNode *index;

    /* Current playback position */
    PlayerState state;
    size_t      cur_track;      /* index into flat track list */
    size_t      cur_frame;      /* playback cursor within cur_buf */

    PcmBuf      cur_buf;
    PcmBuf      next_buf;       /* prefetch; swapped on track end */

    AlsaOut    *alsa;

    /* Wall-clock position tracking: avoids PulseAudio buffer skew.
     * When play starts or resumes, record the monotonic time and the
     * frame offset so that player_get_position() returns wall time,
     * not the (ahead-of-audio) decoded-frame count. */
    struct timespec  play_start_mono;
    uint64_t         frame_at_play_start;
    uint32_t         pos_at_pause_ms;   /* frozen position while paused */
    int              wall_clock_valid;

    /* Mutex guards state, cur_frame, cur_track, state transitions. */
    pthread_mutex_t  lock;
    pthread_cond_t   cond_play;   /* signal to wake the playback thread */
    pthread_cond_t   cond_loaded; /* signal that next_buf is ready */

    /* Thread handles */
    pthread_t  play_tid;
    pthread_t  prefetch_tid;

    /* Subscriber fds for unsolicited events (UI sockets). */
    int        subscribers[MAX_IPC_CLIENTS];
    int        sub_count;

    /* Set to 1 to request all threads exit. */
    volatile int quit;
};

/* ── forward declarations ──────────────────────────────────────────────────── */
static uint32_t get_pos_nolock(const Player *p);
static void     setup_prefetch_thread(void);

/* ── helpers ───────────────────────────────────────────────────────────── */

static const char *state_name(PlayerState s)
{
    switch (s) {
    case PLAYER_IDLE:    return "IDLE";
    case PLAYER_LOADING: return "LOADING";
    case PLAYER_PLAYING: return "PLAYING";
    case PLAYER_PAUSED:  return "PAUSED";
    case PLAYER_ERROR:   return "ERROR";
    default:             return "?";
    }
}

#define SET_STATE(p, new, why) \
    do { \
        LOGI("player: state %s→%s [%s]", state_name((p)->state), state_name(new), (why)); \
        (p)->state = (new); \
    } while (0)

static void pcmbuf_free(PcmBuf *b)
{
    if (b->pcm) {
        decoder_free(b->pcm, b->frames, b->channels);
        b->pcm = NULL;
    }
    b->frames = b->rate = b->channels = 0;
}

static void publish_event(Player *p, const IpcMsg *msg)
{
    for (int i = 0; i < p->sub_count; i++) {
        ssize_t r = send(p->subscribers[i], msg, sizeof(*msg), MSG_DONTWAIT);
        if (r < 0 && errno != EAGAIN)
            LOGD("player: publish to fd %d failed: %s",
                 p->subscribers[i], strerror(errno));
    }
}

static void publish_state(Player *p, PlayerState s)
{
    IpcMsg m = { .type = EVT_STATE, .seq = 0 };
    m.param.player_state = s;
    publish_event(p, &m);
}

static void publish_track(Player *p)
{
    IpcMsg m = { .type = EVT_TRACK, .seq = (uint32_t)p->cur_track };
    memcpy(&m.param.track, &p->cur_buf.info, sizeof(TrackInfo));
    publish_event(p, &m);
}

/* ── prefetch thread ───────────────────────────────────────────────────── */

static void *prefetch_thread(void *arg)
{
    Player *p = arg;
    setup_prefetch_thread();
    while (!p->quit) {
        pthread_mutex_lock(&p->lock);
        /* Wait until told to load the next track */
        while (!p->quit && p->next_buf.pcm)
            pthread_cond_wait(&p->cond_loaded, &p->lock);
        if (p->quit) { pthread_mutex_unlock(&p->lock); break; }
        LOGI("player: prefetch woke: cur_track=%zu next_buf.pcm=%p",
             p->cur_track, (void*)p->next_buf.pcm);

        size_t n = index_track_count(p->index);
        if (n == 0) { pthread_mutex_unlock(&p->lock); continue; }
        size_t next_idx = (p->cur_track + 1) % n;
        pthread_mutex_unlock(&p->lock);

        const TrackInfo *ti = index_get_track(p->index, next_idx);
        if (!ti) continue;

        PcmBuf nb = {0};
        memcpy(&nb.info, ti, sizeof(TrackInfo));
        if (decoder_decode(ti->path, &nb.pcm, &nb.frames, &nb.rate, &nb.channels) != 0) {
            LOGW("player: prefetch failed for %s", ti->path);
            continue;
        }
        if (nb.rate > 0)
            nb.info.duration_ms = (uint32_t)((uint64_t)nb.frames * 1000 / nb.rate);

        pthread_mutex_lock(&p->lock);
        p->next_buf = nb;
        pthread_cond_signal(&p->cond_loaded);
        pthread_mutex_unlock(&p->lock);
    }
    return NULL;
}

/* ── playback thread ───────────────────────────────────────────────────── */

/* Background decode should only consume idle cycles — never preempt audio. */
static void setup_prefetch_thread(void)
{
    struct sched_param sp = { .sched_priority = 0 };
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) != 0)
        LOGW("player: SCHED_IDLE unavailable for prefetch (need kernel support)");
}

/* Pin this thread to CPU1 (first A55) at SCHED_FIFO priority 50. */
static void setup_rt_playback_thread(void)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    struct sched_param sp = { .sched_priority = 50 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        LOGW("player: SCHED_FIFO unavailable (need CAP_SYS_NICE or root)");
}

static void load_track(Player *p, size_t idx)
{
    LOGI("player: load_track idx=%zu (cur_track=%zu state=%s next_buf=%s)",
         idx, p->cur_track, state_name(p->state),
         p->next_buf.pcm ? p->next_buf.info.path : "(empty)");

    const TrackInfo *ti = index_get_track(p->index, idx);
    if (!ti) {
        LOGE("player: load_track idx=%zu — index_get_track returned NULL", idx);
        return;
    }

    /* If the prefetch buffer has exactly the track we need, use it. */
    if (p->next_buf.pcm && strcmp(p->next_buf.info.path, ti->path) == 0) {
        LOGI("player: load_track idx=%zu — fast path (prefetch hit)", idx);
        pcmbuf_free(&p->cur_buf);
        p->cur_buf  = p->next_buf;
        p->next_buf = (PcmBuf){0};
        /* cond_loaded signal deferred until cur_track is updated below so the
         * prefetch thread computes (idx+1) instead of the stale cur_track+1. */
    } else {
        LOGI("player: load_track idx=%zu — slow path (prefetch miss, decoding %s)",
             idx, ti->path);
        /* Decode into locals — never write directly into p->cur_buf while the
         * lock is dropped, preventing a CMD_NEXT use-after-free race. */
        SET_STATE(p, PLAYER_LOADING, "load_track slow path");
        pcmbuf_free(&p->cur_buf);
        /* Publish basic index metadata NOW so the UI shows the track name and
         * artist immediately instead of "Unknown Artist" during the decode. */
        p->cur_track = idx;
        p->cur_frame = 0;
        memcpy(&p->cur_buf.info, ti, sizeof(TrackInfo));
        publish_track(p);
        publish_state(p, PLAYER_LOADING);   /* notify subscribers immediately */
        pthread_mutex_unlock(&p->lock);

        int16_t  *dec_pcm      = NULL;
        size_t    dec_frames   = 0;
        uint32_t  dec_rate     = 0;
        uint32_t  dec_channels = 0;
        int ret = decoder_decode(ti->path, &dec_pcm, &dec_frames,
                                 &dec_rate, &dec_channels);
        pthread_mutex_lock(&p->lock);
        if (ret != 0 || p->quit) {
            if (dec_pcm) decoder_free(dec_pcm, dec_frames, dec_channels);
            if (!p->quit) SET_STATE(p, PLAYER_ERROR, "decoder_decode failed");
            return;
        }
        /* Discard anything a concurrent command loaded while we were decoding */
        pcmbuf_free(&p->cur_buf);
        p->cur_buf.pcm      = dec_pcm;
        p->cur_buf.frames   = dec_frames;
        p->cur_buf.rate     = dec_rate;
        p->cur_buf.channels = dec_channels;
        memcpy(&p->cur_buf.info, ti, sizeof(TrackInfo));
        if (dec_rate > 0)
            p->cur_buf.info.duration_ms =
                (uint32_t)((uint64_t)dec_frames * 1000 / dec_rate);
    }

    p->cur_track = idx;
    p->cur_frame = 0;
    /* Signal prefetch after cur_track is updated so it decodes (idx+1). */
    pthread_cond_signal(&p->cond_loaded);

    /* Keep the ALSA device open across track changes when format is the same
     * (avoids the codec re-init gap that causes an audible click/stutter). */
    if (!p->alsa) {
        LOGI("player: load_track — ALSA not open, opening now");
        p->alsa = alsa_out_open(ALSA_CARD, ALSA_DEVICE,
                                p->cur_buf.rate, p->cur_buf.channels, 16);
        if (!p->alsa) {
            LOGE("player: ALSA open failed");
            SET_STATE(p, PLAYER_ERROR, "ALSA open failed");
            return;
        }
    }
    /* If we were paused, stop the silence-fill thread before the playback
     * thread starts writing real audio.  No-op when silence thread is not
     * running (e.g. CMD_NEXT without a preceding pause). */
    alsa_out_resume(p->alsa);
    SET_STATE(p, PLAYER_PLAYING, "load_track done");

    /* Anchor wall-clock position to the moment the track starts. */
    clock_gettime(CLOCK_MONOTONIC, &p->play_start_mono);
    p->frame_at_play_start = 0;
    p->wall_clock_valid    = 1;

    LOGI("player: track %zu loaded: %zu frames @ %u Hz / %u ch (%.1fs)",
         p->cur_track, p->cur_buf.frames, p->cur_buf.rate, p->cur_buf.channels,
         p->cur_buf.rate ? (double)p->cur_buf.frames / p->cur_buf.rate : 0.0);
    publish_track(p);
    publish_state(p, PLAYER_PLAYING);
}

static void *playback_thread(void *arg)
{
    Player *p = arg;
    setup_rt_playback_thread();

    while (!p->quit) {
        pthread_mutex_lock(&p->lock);
        /* Close ALSA when not playing — only this thread touches p->alsa, so
         * this is the only safe place to close it (no close-while-writing race). */
        if (p->state == PLAYER_IDLE && p->alsa) {
            alsa_out_close(p->alsa);
            p->alsa = NULL;
            LOGI("player: ALSA closed (state=IDLE)");
        }
        if (p->state != PLAYER_PLAYING)
            LOGI("player: playback cond_wait (state=%s)", state_name(p->state));
        while (!p->quit && p->state != PLAYER_PLAYING)
            pthread_cond_wait(&p->cond_play, &p->lock);
        if (p->state == PLAYER_PLAYING)
            LOGD("player: playback woke (state=%s cur_track=%zu cur_frame=%zu)",
                 state_name(p->state), p->cur_track, p->cur_frame);
        if (p->quit) { pthread_mutex_unlock(&p->lock); break; }

        if (!p->cur_buf.pcm) {
            load_track(p, p->cur_track);
            if (p->state != PLAYER_PLAYING) {
                pthread_mutex_unlock(&p->lock);
                continue;
            }
        }

        size_t remaining = p->cur_buf.frames - p->cur_frame;
        if (remaining == 0) {
            /* Track finished — advance to next. */
            size_t total = index_track_count(p->index);
            if (total == 0) {
                SET_STATE(p, PLAYER_IDLE, "no tracks");
                pthread_mutex_unlock(&p->lock);
                continue;
            }
            size_t next = (p->cur_track + 1) % total;
            LOGI("player: track %zu finished, advancing to track %zu", p->cur_track, next);
            load_track(p, next);
            LOGI("player: after auto-advance load_track: state=%s", state_name(p->state));
            if (p->state == PLAYER_ERROR) {
                /* Decode failure on auto-advance; reset to IDLE so CMD_PLAY can recover */
                SET_STATE(p, PLAYER_IDLE, "auto-advance failed → IDLE");
                publish_state(p, PLAYER_IDLE);
            } else if (p->state == PLAYER_PLAYING) {
                /* Immediately signal ourselves so the loop doesn't cond_wait */
                pthread_cond_signal(&p->cond_play);
            }
            pthread_mutex_unlock(&p->lock);
            continue;
        }

        size_t chunk = (remaining < PERIOD_FRAMES) ? remaining : PERIOD_FRAMES;
        /* Copy PCM before dropping the lock — prevents use-after-free if
         * CMD_NEXT/PREV calls pcmbuf_free mid-write.  static is safe: exactly
         * one playback thread exists, so there is no reentrancy concern. */
        static int16_t  pcm_local[PERIOD_FRAMES * 2];
        memcpy(pcm_local,
               p->cur_buf.pcm + p->cur_frame * p->cur_buf.channels,
               chunk * p->cur_buf.channels * sizeof(int16_t));
        uint32_t wr_rate     = p->cur_buf.rate;
        uint32_t wr_channels = p->cur_buf.channels;
        AlsaOut *wr_alsa     = p->alsa;
        p->cur_frame        += chunk;
        pthread_mutex_unlock(&p->lock);

        if (!wr_alsa) {
            /* Open ALSA only when still playing (avoids reopen after CMD_STOP). */
            pthread_mutex_lock(&p->lock);
            if (p->state == PLAYER_PLAYING) {
                p->alsa = alsa_out_open(ALSA_CARD, ALSA_DEVICE,
                                        wr_rate, wr_channels, 16);
                if (!p->alsa) SET_STATE(p, PLAYER_ERROR, "ALSA open failed");
            }
            pthread_mutex_unlock(&p->lock);
            continue;
        }
        if (alsa_out_write(wr_alsa, pcm_local, chunk) != 0) {
            LOGE("player: alsa_out_write failed; reopening (rate=%u ch=%u)",
                 wr_rate, wr_channels);
            pthread_mutex_lock(&p->lock);
            /* Only close if CMD_STOP hasn't already replaced the handle. */
            if (p->alsa == wr_alsa) { alsa_out_close(p->alsa); p->alsa = NULL; }
            if (p->state == PLAYER_PLAYING) {
                p->alsa = alsa_out_open(ALSA_CARD, ALSA_DEVICE,
                                        wr_rate, wr_channels, 16);
                if (!p->alsa) SET_STATE(p, PLAYER_ERROR, "ALSA reopen failed");
            }
            pthread_mutex_unlock(&p->lock);
        }
    }

    if (p->alsa) { alsa_out_close(p->alsa); p->alsa = NULL; }
    return NULL;
}

/* ── public API ────────────────────────────────────────────────────────── */

Player *player_create(IndexNode *index_root)
{
    Player *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->index     = index_root;
    p->state     = PLAYER_IDLE;
    p->cur_track = 0;

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond_play,   NULL);
    pthread_cond_init(&p->cond_loaded, NULL);

    for (int i = 0; i < MAX_IPC_CLIENTS; i++)
        p->subscribers[i] = -1;

    pthread_create(&p->play_tid,     NULL, playback_thread, p);
    pthread_create(&p->prefetch_tid, NULL, prefetch_thread,  p);
    return p;
}

void player_destroy(Player *p)
{
    if (!p) return;
    p->quit = 1;
    pthread_cond_broadcast(&p->cond_play);
    pthread_cond_broadcast(&p->cond_loaded);
    pthread_join(p->play_tid,     NULL);
    pthread_join(p->prefetch_tid, NULL);
    pcmbuf_free(&p->cur_buf);
    pcmbuf_free(&p->next_buf);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond_play);
    pthread_cond_destroy(&p->cond_loaded);
    free(p);
}

IpcMsg player_handle_cmd(Player *p, const IpcMsg *cmd)
{
    IpcMsg reply = { .seq = cmd->seq };
    pthread_mutex_lock(&p->lock);

    LOGI("player: cmd 0x%02x (state=%s cur_track=%zu)",
         (unsigned)cmd->type, state_name(p->state), p->cur_track);

    switch ((IpcMsgType)cmd->type) {
    case CMD_PLAY:
        /* Accept PLAY from PAUSED, IDLE, and ERROR (error recovery). */
        if (p->state == PLAYER_PAUSED || p->state == PLAYER_IDLE ||
            p->state == PLAYER_ERROR) {
            if (p->alsa) alsa_out_resume(p->alsa);
            /* Re-anchor wall clock at the paused position so the position
             * counter continues from where it stopped, not from 0. */
            if (p->cur_buf.rate) {
                p->frame_at_play_start = (uint64_t)p->pos_at_pause_ms
                                        * p->cur_buf.rate / 1000;
            }
            clock_gettime(CLOCK_MONOTONIC, &p->play_start_mono);
            p->wall_clock_valid = 1;
            SET_STATE(p, PLAYER_PLAYING, "CMD_PLAY");
            pthread_cond_signal(&p->cond_play);
        }
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_PAUSE:
        if (p->state == PLAYER_PLAYING) {
            /* Freeze the position at the current wall-clock reading before
             * stopping so the display holds the correct timestamp. */
            p->pos_at_pause_ms = get_pos_nolock(p);
            SET_STATE(p, PLAYER_PAUSED, "CMD_PAUSE");
            if (p->alsa) alsa_out_pause(p->alsa);
            publish_state(p, PLAYER_PAUSED);
        }
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_STOP:
        SET_STATE(p, PLAYER_IDLE, "CMD_STOP");
        p->cur_frame = 0;
        /* ALSA is closed by the playback thread once it observes IDLE state —
         * closing here would race with an in-progress fwrite. */
        publish_state(p, PLAYER_IDLE);
        reply.type = EVT_STATE;
        reply.param.player_state = PLAYER_IDLE;
        break;

    case CMD_NEXT: {
        size_t n = index_track_count(p->index);
        if (n > 0) {
            size_t next = (p->cur_track + 1) % n;
            load_track(p, next);
            pthread_cond_signal(&p->cond_play);
        }
        /* EVT_TRACK is already published to all subscribers by load_track.
         * Sending another EVT_TRACK as a direct reply would cause the UI to
         * reset position_ms=0 a second time, after the timer has already
         * advanced the bar forward — the jiggle.  Reply with EVT_STATE only. */
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;
    }
    case CMD_PREV: {
        size_t n = index_track_count(p->index);
        if (n > 0) {
            size_t prev = (p->cur_track + n - 1) % n;
            load_track(p, prev);
            pthread_cond_signal(&p->cond_play);
        }
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;
    }
    case CMD_SEEK:
        if (p->cur_buf.pcm && p->cur_buf.rate > 0) {
            size_t target = (size_t)(
                (uint64_t)cmd->param.seek_ms * p->cur_buf.rate / 1000);
            if (target > p->cur_buf.frames) target = p->cur_buf.frames;
            p->cur_frame = target;
            /* Re-anchor wall clock so position continues from the seek point. */
            p->frame_at_play_start = target;
            clock_gettime(CLOCK_MONOTONIC, &p->play_start_mono);
            p->pos_at_pause_ms = cmd->param.seek_ms;
        }
        reply.type = EVT_POSITION;
        reply.param.position_ms = cmd->param.seek_ms;
        break;

    case CMD_LOAD_TRACK:
        load_track(p, cmd->param.track_idx);
        pthread_cond_signal(&p->cond_play);
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_GET_STATE:
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_GET_INDEX:
        reply.type = EVT_INDEX_READY;
        reply.param.track_count = (uint32_t)index_track_count(p->index);
        break;

    case CMD_GET_TRACK_INFO: {
        const TrackInfo *ti = index_get_track(p->index, cmd->param.track_idx);
        if (ti) {
            reply.type = EVT_TRACK_INFO;
            reply.seq  = cmd->param.track_idx;
            memcpy(&reply.param.track, ti, sizeof(TrackInfo));
        } else {
            reply.type = EVT_ERROR;
            reply.param.error_code = -ENOENT;
        }
        break;
    }

    default:
        reply.type = EVT_ERROR;
        reply.param.error_code = -EINVAL;
        break;
    }

    pthread_mutex_unlock(&p->lock);
    return reply;
}

/* Lock-free inner version — caller MUST hold p->lock. */
static uint32_t get_pos_nolock(const Player *p)
{
    if (p->state != PLAYER_PLAYING && p->state != PLAYER_PAUSED)
        return UINT32_MAX;
    if (!p->cur_buf.rate) return 0;

    if (p->state == PLAYER_PAUSED)
        return p->pos_at_pause_ms;

    if (!p->wall_clock_valid) {
        /* Fallback: frame-count based (accurate on real hardware, skews on sim) */
        return (uint32_t)((uint64_t)p->cur_frame * 1000 / p->cur_buf.rate);
    }

    /* Wall-clock based: tracks real elapsed time from play-start regardless
     * of how fast PulseAudio drained the write buffer. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec_diff = (int64_t)now.tv_sec  - (int64_t)p->play_start_mono.tv_sec;
    int64_t ns_diff  = (int64_t)now.tv_nsec - (int64_t)p->play_start_mono.tv_nsec;
    if (ns_diff < 0) { sec_diff--; ns_diff += 1000000000LL; }
    uint64_t elapsed_ns = (uint64_t)(sec_diff < 0 ? 0 : sec_diff) * 1000000000ULL
                        + (uint64_t)ns_diff;
    uint64_t elapsed_ms = elapsed_ns / 1000000ULL;
    uint32_t pos_ms = (uint32_t)(
        (uint64_t)p->frame_at_play_start * 1000 / p->cur_buf.rate + elapsed_ms);
    uint32_t dur_ms = p->cur_buf.info.duration_ms;
    if (dur_ms && pos_ms > dur_ms) pos_ms = dur_ms;
    return pos_ms;
}

/* Public API — acquires p->lock so callers (e.g. the timer thread) don't race
 * with load_track updating play_start_mono / frame_at_play_start. */
uint32_t player_get_position(Player *p)
{
    pthread_mutex_lock(&p->lock);
    uint32_t pos = get_pos_nolock(p);
    pthread_mutex_unlock(&p->lock);
    return pos;
}

int player_subscribe(Player *p, int fd)
{
    pthread_mutex_lock(&p->lock);
    if (p->sub_count >= MAX_IPC_CLIENTS) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->subscribers[p->sub_count++] = fd;
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void player_unsubscribe(Player *p, int fd)
{
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < p->sub_count; i++) {
        if (p->subscribers[i] == fd) {
            p->subscribers[i] = p->subscribers[--p->sub_count];
            p->subscribers[p->sub_count] = -1;
            break;
        }
    }
    pthread_mutex_unlock(&p->lock);
}
