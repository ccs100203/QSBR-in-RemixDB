/*
 * Copyright (c) 2016--2021  Wu, Xingbo <wuxb45@gmail.com>
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#pragma once

// includes {{{
// C headers
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// POSIX headers
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

// Linux headers
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

// SIMD
#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(__aarch64__)
#include <arm_acle.h>
#include <arm_neon.h>
#endif
// }}} includes

#ifdef __cplusplus
extern "C" {
#endif

// defs {{{
#define likely(____x____) __builtin_expect(____x____, 1)
#define unlikely(____x____) __builtin_expect(____x____, 0)

// ansi colors
// 3X:fg; 4X:bg; 9X:light fg; 10X:light bg;
// X can be one of the following colors:
// 0:black;   1:red;     2:green;  3:yellow;
// 4:blue;    5:magenta; 6:cyan;   7:white;
#define TERMCLR(____code____) "\x1b[" #____code____ "m"
// }}} defs

// const {{{
#define PGSZ ((4096lu))
// }}} const

extern void cpu_pause(void);

extern void cpu_cfence(void);

#ifndef NDEBUG
extern void debug_assert(const bool v);
#else
#define debug_assert(expr) ((void) 0)
#endif

extern void *yalloc(const size_t size);

extern uint32_t crc32c_u64(const uint32_t crc, const uint64_t v);

// qsbr {{{
// QSBR vs EBR (Quiescent-State vs Epoch Based Reclaimation)
// QSBR: readers just use qsbr_update -> qsbr_update -> ... repeatedly
// EBR: readers use qsbr_update -> qsbr_park -> qsbr_resume -> qsbr_update ->
// ... The advantage of EBR is qsbr_park can happen much earlier than the next
// qsbr_update The disadvantage is the extra cost, a pair of park/resume is used
// in every iteration
struct qsbr;
struct qsbr_ref {
#ifdef QSBR_DEBUG
    uint64_t debug[16];
#endif
    uint64_t opaque[3];
};

extern struct qsbr *qsbr_create(void);

// every READER accessing the shared data must first register itself with the
// qsbr
extern bool qsbr_register(struct qsbr *const q, struct qsbr_ref *const qref);

extern void qsbr_unregister(struct qsbr *const q, struct qsbr_ref *const qref);

// For READER: mark the beginning of critical section; like rcu_read_lock()
extern void qsbr_update(struct qsbr_ref *const qref, const uint64_t v);

// temporarily stop access the shared data to avoid blocking writers
// READER can use qsbr_park (like rcu_read_unlock()) in conjunction with
// qsbr_update qsbr_park is roughly equivalent to qsbr_unregister, but faster
extern void qsbr_park(struct qsbr_ref *const qref);

// undo the effect of qsbr_park; must use it between qsbr_park and qsbr_update
// qsbr_resume is roughly equivalent to qsbr_register, but faster
extern void qsbr_resume(struct qsbr_ref *const qref);

// WRITER: wait until all the readers have announced v=target with qsbr_update
extern void qsbr_wait(struct qsbr *const q, const uint64_t target);

extern void qsbr_destroy(struct qsbr *const q);
// }}} qsbr

#ifdef __cplusplus
}
#endif
// vim:fdm=marker
