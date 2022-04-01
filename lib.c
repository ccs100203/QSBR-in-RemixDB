/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#define _GNU_SOURCE

// headers {{{
#include "lib.h"
#include <assert.h>
#include <execinfo.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>  // va_start
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include "ctypes.h"

#if defined(__linux__)
#include <linux/fs.h>
#include <malloc.h>  // malloc_usable_size
#elif defined(__APPLE__) && defined(__MACH__)
#include <malloc/malloc.h>
#include <sys/disk.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#include <sys/disk.h>
#endif  // OS

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
// }}} headers

inline void cpu_pause(void)
{
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    // nop
#endif
}

// compiler fence
inline void cpu_cfence(void)
{
    atomic_thread_fence(MO_ACQ_REL);
}

#ifndef NDEBUG
#define debug_assert(v) assert(v)
#endif

// alloc cache-line aligned address
void *yalloc(const size_t size)
{
    void *p;
    return (posix_memalign(&p, 64, size) == 0) ? p : NULL;
}

inline u32 crc32c_u64(const u32 crc, const u64 v)
{
#if defined(__x86_64__)
    return (u32) _mm_crc32_u64(crc, v);
#elif defined(__aarch64__)
    return (u32) __crc32cd(crc, v);
#endif
}

// qsbr {{{
// shard capacity; valid values are 3*8-1 == 23; 5*8-1 == 39; 7*8-1 == 55
#define QSBR_STATES_NR ((23))
#define QSBR_SHARD_BITS ((5))  // 2^n shards
#define QSBR_SHARD_NR (((1u) << QSBR_SHARD_BITS))
#define QSBR_SHARD_MASK ((QSBR_SHARD_NR - 1))

struct qsbr_ref_real {
#ifdef QSBR_DEBUG
    pthread_t ptid;  // 8
    u32 status;      // 4
    u32 nbt;         // 4 (number of backtrace frames)
#define QSBR_DEBUG_BTNR ((14))
    void *backtrace[QSBR_DEBUG_BTNR];
#endif
    volatile au64 qstate;                  // user updates it
    struct qsbr_ref_real *volatile *pptr;  // internal only
    struct qsbr_ref_real *park;
};

static_assert(sizeof(struct qsbr_ref) == sizeof(struct qsbr_ref_real),
              "sizeof qsbr_ref");

// Quiescent-State-Based Reclamation RCU
struct qsbr {
    struct qsbr_ref_real target;
    u64 padding0[5];
    struct qshard {
        au64 bitmap;
        struct qsbr_ref_real *volatile ptrs[QSBR_STATES_NR];
    } shards[QSBR_SHARD_NR];
};

struct qsbr *qsbr_create(void)
{
    struct qsbr *const q = yalloc(sizeof(*q));
    memset(q, 0, sizeof(*q));
    return q;
}

static inline struct qshard *qsbr_shard(struct qsbr *const q, void *const ptr)
{
    const u32 sid = crc32c_u64(0, (u64) ptr) & QSBR_SHARD_MASK;
    debug_assert(sid < QSBR_SHARD_NR);
    return &(q->shards[sid]);
}

static inline void qsbr_write_qstate(struct qsbr_ref_real *const ref,
                                     const u64 v)
{
    atomic_store_explicit(&ref->qstate, v, MO_RELAXED);
}

bool qsbr_register(struct qsbr *const q, struct qsbr_ref *const qref)
{
    struct qsbr_ref_real *const ref = (typeof(ref)) qref;
    struct qshard *const shard = qsbr_shard(q, ref);
    qsbr_write_qstate(ref, 0);

    do {
        u64 bits = atomic_load_explicit(&shard->bitmap, MO_CONSUME);
        const u32 pos = (u32) __builtin_ctzl(~bits);
        if (unlikely(pos >= QSBR_STATES_NR))
            return false;

        const u64 bits1 = bits | (1lu << pos);
        if (atomic_compare_exchange_weak_explicit(&shard->bitmap, &bits, bits1,
                                                  MO_ACQUIRE, MO_RELAXED)) {
            shard->ptrs[pos] = ref;

            ref->pptr = &(shard->ptrs[pos]);
            ref->park = &q->target;
#ifdef QSBR_DEBUG
            ref->ptid = (u64) pthread_self();
            ref->tid = 0;
            ref->status = 1;
            ref->nbt = backtrace(ref->backtrace, QSBR_DEBUG_BTNR);
#endif
            return true;
        }
    } while (true);
}

void qsbr_unregister(struct qsbr *const q, struct qsbr_ref *const qref)
{
    struct qsbr_ref_real *const ref = (typeof(ref)) qref;
    struct qshard *const shard = qsbr_shard(q, ref);
    const u32 pos = (u32)(ref->pptr - shard->ptrs);
    debug_assert(pos < QSBR_STATES_NR);
    debug_assert(shard->bitmap & (1lu << pos));

    shard->ptrs[pos] = &q->target;
    (void) atomic_fetch_and_explicit(&shard->bitmap, ~(1lu << pos), MO_RELEASE);
#ifdef QSBR_DEBUG
    ref->tid = 0;
    ref->ptid = 0;
    ref->status = 0xffff;  // unregistered
    ref->nbt = 0;
#endif
    ref->pptr = NULL;
    // wait for qsbr_wait to leave if it's working on the shard
    while (atomic_load_explicit(&shard->bitmap, MO_CONSUME) >> 63)
        cpu_pause();
}

inline void qsbr_update(struct qsbr_ref *const qref, const u64 v)
{
    struct qsbr_ref_real *const ref = (typeof(ref)) qref;
    debug_assert((*ref->pptr) == ref);  // must be unparked
    // rcu update does not require release or acquire order
    qsbr_write_qstate(ref, v);
}

inline void qsbr_park(struct qsbr_ref *const qref)
{
    cpu_cfence();
    struct qsbr_ref_real *const ref = (typeof(ref)) qref;
    *ref->pptr = ref->park;
#ifdef QSBR_DEBUG
    ref->status = 0xfff;  // parked
#endif
}

inline void qsbr_resume(struct qsbr_ref *const qref)
{
    struct qsbr_ref_real *const ref = (typeof(ref)) qref;
    *ref->pptr = ref;
#ifdef QSBR_DEBUG
    ref->status = 0xf;  // resumed
#endif
    cpu_cfence();
}

// waiters needs external synchronization
void qsbr_wait(struct qsbr *const q, const u64 target)
{
    cpu_cfence();
    qsbr_write_qstate(&q->target, target);
    u64 cbits = 0;           // check-bits; each bit corresponds to a shard
    u64 bms[QSBR_SHARD_NR];  // copy of all bitmap
    // take an unsafe snapshot of active users
    for (u32 i = 0; i < QSBR_SHARD_NR; i++) {
        bms[i] = atomic_load_explicit(&q->shards[i].bitmap, MO_CONSUME);
        if (bms[i])
            cbits |= (1lu << i);  // set to 1 if [i] has ptrs
    }

    while (cbits) {
        for (u64 ctmp = cbits; ctmp; ctmp &= (ctmp - 1)) {
            // shard id
            const u32 i = (u32) __builtin_ctzl(ctmp);
            struct qshard *const shard = &(q->shards[i]);
            const u64 bits1 = atomic_fetch_or_explicit(&(shard->bitmap),
                                                       1lu << 63, MO_ACQUIRE);
            for (u64 bits = bms[i]; bits; bits &= (bits - 1)) {
                const u64 bit = bits & -bits;  // extract lowest bit
                if (((bits1 & bit) == 0) ||
                    (atomic_load_explicit(
                         &(shard->ptrs[__builtin_ctzl(bit)]->qstate),
                         MO_CONSUME) == target))
                    bms[i] &= ~bit;
            }
            (void) atomic_fetch_and_explicit(&(shard->bitmap), ~(1lu << 63),
                                             MO_RELEASE);
            if (bms[i] == 0)
                cbits &= ~(1lu << i);
        }
#if defined(CORR)
        corr_yield();
#endif
    }
    debug_assert(cbits == 0);
    cpu_cfence();
}

void qsbr_destroy(struct qsbr *const q)
{
    if (q)
        free(q);
}
#undef QSBR_STATES_NR
#undef QSBR_BITMAP_NR
// }}} qsbr

// vim:fdm=marker
