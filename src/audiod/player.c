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

#define ALSA_CARD    0
#define ALSA_DEVICE  0
#define MUSIC_DIR    "/sdcard/Music"

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

/* ── helpers ───────────────────────────────────────────────────────────── */

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
    IpcMsg m = { .type = EVT_TRACK, .seq = 0 };
    memcpy(&m.param.track, &p->cur_buf.info, sizeof(TrackInfo));
    publish_event(p, &m);
}

/* ── prefetch thread ───────────────────────────────────────────────────── */

static void *prefetch_thread(void *arg)
{
    Player *p = arg;
    while (!p->quit) {
        pthread_mutex_lock(&p->lock);
        /* Wait until told to load the next track */
        while (!p->quit && p->next_buf.pcm)
            pthread_cond_wait(&p->cond_loaded, &p->lock);
        if (p->quit) { pthread_mutex_unlock(&p->lock); break; }

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

        pthread_mutex_lock(&p->lock);
        p->next_buf = nb;
        pthread_cond_signal(&p->cond_loaded);
        pthread_mutex_unlock(&p->lock);
    }
    return NULL;
}

/* ── playback thread ───────────────────────────────────────────────────── */

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
    const TrackInfo *ti = index_get_track(p->index, idx);
    if (!ti) return;

    /* If the prefetch buffer has exactly the track we need, use it. */
    if (p->next_buf.pcm && strcmp(p->next_buf.info.path, ti->path) == 0) {
        pcmbuf_free(&p->cur_buf);
        p->cur_buf   = p->next_buf;
        p->next_buf  = (PcmBuf){0};
        pthread_cond_signal(&p->cond_loaded); /* wake prefetch */
    } else {
        pcmbuf_free(&p->cur_buf);
        memcpy(&p->cur_buf.info, ti, sizeof(TrackInfo));
        p->state = PLAYER_LOADING;
        pthread_mutex_unlock(&p->lock);

        if (decoder_decode(ti->path,
                           &p->cur_buf.pcm, &p->cur_buf.frames,
                           &p->cur_buf.rate, &p->cur_buf.channels) != 0) {
            p->cur_buf.pcm = NULL;
            pthread_mutex_lock(&p->lock);
            p->state = PLAYER_ERROR;
            return;
        }
        pthread_mutex_lock(&p->lock);
    }

    p->cur_track = idx;
    p->cur_frame = 0;

    /* Re-open ALSA if sample rate / channels changed. */
    if (p->alsa) {
        alsa_out_close(p->alsa);
        p->alsa = NULL;
    }
    p->alsa = alsa_out_open(ALSA_CARD, ALSA_DEVICE,
                             p->cur_buf.rate, p->cur_buf.channels, 16);
    if (!p->alsa) {
        LOGE("player: ALSA open failed");
        p->state = PLAYER_ERROR;
        return;
    }
    p->state = PLAYER_PLAYING;
    publish_track(p);
}

static void *playback_thread(void *arg)
{
    Player *p = arg;
    setup_rt_playback_thread();

    const size_t PERIOD_FRAMES = 4096;

    while (!p->quit) {
        pthread_mutex_lock(&p->lock);
        while (!p->quit && p->state != PLAYER_PLAYING)
            pthread_cond_wait(&p->cond_play, &p->lock);
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
            if (total == 0) { p->state = PLAYER_IDLE; pthread_mutex_unlock(&p->lock); continue; }
            size_t next = (p->cur_track + 1) % total;
            load_track(p, next);
            pthread_mutex_unlock(&p->lock);
            continue;
        }

        size_t chunk = (remaining < PERIOD_FRAMES) ? remaining : PERIOD_FRAMES;
        const int16_t *src = p->cur_buf.pcm +
                             p->cur_frame * p->cur_buf.channels;
        p->cur_frame += chunk;
        pthread_mutex_unlock(&p->lock);

        /* pcm_writei blocks until hardware consumes the period — this is the
         * only CPU activity during playback; everything else sleeps. */
        if (alsa_out_write(p->alsa, src, chunk) != 0) {
            LOGE("player: alsa_out_write failed; reopening");
            pthread_mutex_lock(&p->lock);
            alsa_out_close(p->alsa);
            p->alsa = alsa_out_open(ALSA_CARD, ALSA_DEVICE,
                                     p->cur_buf.rate, p->cur_buf.channels, 16);
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

    switch ((IpcMsgType)cmd->type) {
    case CMD_PLAY:
        if (p->state == PLAYER_PAUSED || p->state == PLAYER_IDLE) {
            if (p->alsa) alsa_out_resume(p->alsa);
            p->state = PLAYER_PLAYING;
            pthread_cond_signal(&p->cond_play);
        }
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_PAUSE:
        if (p->state == PLAYER_PLAYING) {
            p->state = PLAYER_PAUSED;
            if (p->alsa) alsa_out_pause(p->alsa);
            publish_state(p, PLAYER_PAUSED);
        }
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_STOP:
        p->state = PLAYER_IDLE;
        p->cur_frame = 0;
        if (p->alsa) { alsa_out_close(p->alsa); p->alsa = NULL; }
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
        reply.type = EVT_TRACK;
        memcpy(&reply.param.track, &p->cur_buf.info, sizeof(TrackInfo));
        break;
    }
    case CMD_PREV: {
        size_t n = index_track_count(p->index);
        if (n > 0) {
            size_t prev = (p->cur_track + n - 1) % n;
            load_track(p, prev);
            pthread_cond_signal(&p->cond_play);
        }
        reply.type = EVT_TRACK;
        memcpy(&reply.param.track, &p->cur_buf.info, sizeof(TrackInfo));
        break;
    }
    case CMD_SEEK:
        if (p->cur_buf.pcm && p->cur_buf.rate > 0) {
            size_t target = (size_t)(
                (uint64_t)cmd->param.seek_ms * p->cur_buf.rate / 1000);
            if (target > p->cur_buf.frames) target = p->cur_buf.frames;
            p->cur_frame = target;
        }
        reply.type = EVT_POSITION;
        reply.param.position_ms = cmd->param.seek_ms;
        break;

    case CMD_LOAD_TRACK:
        load_track(p, cmd->param.track_idx);
        pthread_cond_signal(&p->cond_play);
        reply.type = EVT_TRACK;
        memcpy(&reply.param.track, &p->cur_buf.info, sizeof(TrackInfo));
        break;

    case CMD_GET_STATE:
        reply.type = EVT_STATE;
        reply.param.player_state = p->state;
        break;

    case CMD_GET_INDEX:
        reply.type = EVT_INDEX_READY;
        reply.param.track_count = (uint32_t)index_track_count(p->index);
        break;

    default:
        reply.type = EVT_ERROR;
        reply.param.error_code = -EINVAL;
        break;
    }

    pthread_mutex_unlock(&p->lock);
    return reply;
}

uint32_t player_get_position(const Player *p)
{
    if (p->state != PLAYER_PLAYING && p->state != PLAYER_PAUSED)
        return UINT32_MAX;
    if (!p->cur_buf.rate) return 0;
    /* Intentionally unsynchronised read of cur_frame — a stale value is fine
     * for a once-per-second UI position tick. */
    return (uint32_t)(
        (uint64_t)p->cur_frame * 1000 / p->cur_buf.rate);
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
