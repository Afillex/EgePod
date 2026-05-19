#pragma once
/* Lock-free single-producer / single-consumer ring buffer.
 * capacity must be a power of 2.
 * Safe for exactly one writer thread and one reader thread concurrently. */
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint8_t         *buf;
    size_t           capacity;   /* power of 2 */
    size_t           mask;
    _Atomic size_t   write_idx;
    _Atomic size_t   read_idx;
} RingBuf;

static inline int ringbuf_init(RingBuf *rb, size_t capacity)
{
    if (!capacity || (capacity & (capacity - 1))) return -1; /* not power of 2 */
    rb->buf = malloc(capacity);
    if (!rb->buf) return -1;
    rb->capacity = capacity;
    rb->mask     = capacity - 1;
    atomic_store_explicit(&rb->write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx,  0, memory_order_relaxed);
    return 0;
}

static inline void ringbuf_destroy(RingBuf *rb)
{
    free(rb->buf);
    rb->buf = NULL;
}

static inline void ringbuf_reset(RingBuf *rb)
{
    atomic_store_explicit(&rb->write_idx, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx,  0, memory_order_relaxed);
}

static inline size_t ringbuf_writable(const RingBuf *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t r = atomic_load_explicit(&rb->read_idx,  memory_order_acquire);
    return rb->capacity - (w - r);
}

static inline size_t ringbuf_readable(const RingBuf *rb)
{
    size_t r = atomic_load_explicit(&rb->read_idx,  memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    return w - r;
}

/* Returns bytes written; 0 if insufficient space. */
static inline size_t ringbuf_write(RingBuf *rb, const void *data, size_t len)
{
    if (ringbuf_writable(rb) < len) return 0;
    size_t w_abs = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t w     = w_abs & rb->mask;
    size_t tail  = rb->capacity - w;
    if (len <= tail) {
        memcpy(rb->buf + w, data, len);
    } else {
        memcpy(rb->buf + w,    data,          tail);
        memcpy(rb->buf,        (const uint8_t *)data + tail, len - tail);
    }
    atomic_store_explicit(&rb->write_idx, w_abs + len, memory_order_release);
    return len;
}

/* Returns bytes read; 0 if insufficient data. */
static inline size_t ringbuf_read(RingBuf *rb, void *data, size_t len)
{
    if (ringbuf_readable(rb) < len) return 0;
    size_t r_abs = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    size_t r     = r_abs & rb->mask;
    size_t tail  = rb->capacity - r;
    if (len <= tail) {
        memcpy(data, rb->buf + r, len);
    } else {
        memcpy(data,                   rb->buf + r, tail);
        memcpy((uint8_t *)data + tail, rb->buf,     len - tail);
    }
    atomic_store_explicit(&rb->read_idx, r_abs + len, memory_order_release);
    return len;
}
