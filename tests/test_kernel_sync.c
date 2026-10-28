// A direct kernel test: one thread waits in svcWaitProcessWideKeyAtomic, another signals it. The key is published
// before the mutex is released; a signal that arrives in that window must not be lost.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "kernel.h"

// kernel.c, with its statics visible to this test.
#include "../runtime/kernel.c"

// Data the kernel references but this path never reads.
Module g_modules[8];
int g_nmodules;
int rt_romfs_files = -1;
const NxGame nx_game = {0};

// Platform stubs the sync path needs (the rest is not called).
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) { (void)t; }

static _Atomic int ready, done;
static uint32_t mutex, key;
static KThread waiter, signaler;
static int result;

// Negative control: the old order (release the mutex, then publish the key, then block) lets a signaler read the key
// as 0 and skip the wakeup. The waiter must time out; the test proves it can detect the bug.
static _Atomic int old_ready, old_done, old_missed;
static uint32_t old_mutex, old_key;
static KThread old_waiter;
static int old_result;

static void *old_wait_thread(void *arg) {
    (void)arg;
    t_cur = &old_waiter;
    pthread_mutex_lock(&g_klock);
    __atomic_store_n(&old_mutex, 0, __ATOMIC_SEQ_CST);  // release first (the bug)
    old_ready = 1;
    usleep(2000);
    __atomic_store_n(&old_key, 1, __ATOMIC_SEQ_CST);  // publish after the release
    old_waiter.wait_kind = WAIT_CONDVAR;
    old_waiter.wait_addr = (uint64_t)&old_key;
    old_waiter.wait_mutex = (uint64_t)&old_mutex;
    old_waiter.wait_tag = old_waiter.handle;
    old_result = (int)block_current(100000000);
    pthread_mutex_unlock(&g_klock);
    old_done = 1;
    return NULL;
}

static void *old_signal_thread(void *arg) {
    (void)arg;
    while (!old_ready) sched_yield();
    while (__atomic_load_n(&old_mutex, __ATOMIC_ACQUIRE) != 0) sched_yield();
    usleep(1000);
    if (__atomic_load_n(&old_key, __ATOMIC_ACQUIRE) == 0) old_missed = 1;  // saw no waiter: skipped the signal
    return NULL;
}

static void *wait_thread(void *arg) {
    (void)arg;
    t_cur = &waiter;
    ready = 1;
    result = (int)svc_wait_process_wide_key((uint64_t)&mutex, (uint64_t)&key, waiter.handle, 2000000000ll);
    done = 1;
    return NULL;
}

static void *signal_thread(void *arg) {
    (void)arg;
    t_cur = &signaler;
    while (!ready) sched_yield();
    // Signal as soon as the key is published; the kernel must still see the waiter (or the wakeup is lost).
    while (!__atomic_load_n(&key, __ATOMIC_ACQUIRE)) sched_yield();
    svc_signal_process_wide_key((uint64_t)&key, 1);
    return NULL;
}

int main(void) {
    waiter.handle = 0x101;
    waiter.prio = 44;
    waiter.started = true;
    pthread_cond_init(&waiter.cond, NULL);
    signaler.handle = 0x102;
    signaler.prio = 44;
    pthread_mutex_init(&g_klock, NULL);

    for (int i = 0; i < 200; i++) {
        mutex = 0;
        key = 0;
        ready = done = 0;
        result = -1;
        pthread_t w, s;
        pthread_create(&w, NULL, wait_thread, NULL);
        pthread_create(&s, NULL, signal_thread, NULL);
        pthread_join(s, NULL);
        pthread_join(w, NULL);
        if (!done || result != 0) {
            fprintf(stderr, "iteration %d: done %d result %d key %u mutex %u\n", i, done, result, key, mutex);
            return 1;
        }
    }
    // The negative control: the old order loses the wakeup, and the waiter times out.
    old_waiter.handle = 0x103;
    old_waiter.prio = 44;
    old_waiter.started = true;
    pthread_cond_init(&old_waiter.cond, NULL);
    old_mutex = 0x104;  // held by "the game"
    old_key = 0;
    old_ready = old_done = old_missed = 0;
    old_result = -1;
    pthread_t ow, os;
    pthread_create(&ow, NULL, old_wait_thread, NULL);
    pthread_create(&os, NULL, old_signal_thread, NULL);
    pthread_join(os, NULL);
    pthread_join(ow, NULL);
    if (!old_done || !old_missed || old_result != (int)R_TIMEOUT) {
        fprintf(stderr, "negative control: done %d missed %d result %d\n", old_done, old_missed, old_result);
        return 2;
    }

    puts("test_kernel_sync: ok");
    return 0;
}
