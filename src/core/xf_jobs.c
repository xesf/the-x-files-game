/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "xf_jobs.h"

#include <stdlib.h>

#define MAX_WORKERS 7

/* --- Threads and semaphores of each system ----------------------------- */

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
typedef SceUID sema;
static int sema_init(sema *s) { return (*s = sceKernelCreateSema("xfjob", 0, 0, 0x7FFF, NULL)) < 0 ? -1 : 0; }
static void sema_wait(sema *s) { sceKernelWaitSema(*s, 1, NULL); }
static void sema_post(sema *s) { sceKernelSignalSema(*s, 1); }
static int system_cores(void) { return 3; } /* cores 0-2 run applications */
#define HAVE_THREADS 1
#elif defined(_WIN32)
#include <windows.h>
typedef HANDLE sema;
static int sema_init(sema *s) { return (*s = CreateSemaphore(NULL, 0, 0x7FFF, NULL)) ? 0 : -1; }
static void sema_wait(sema *s) { WaitForSingleObject(*s, INFINITE); }
static void sema_post(sema *s) { ReleaseSemaphore(*s, 1, NULL); }
static int system_cores(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#define HAVE_THREADS 1
#elif defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int n;
} sema; /* macOS has no unnamed POSIX semaphores */
static int sema_init(sema *s)
{
    s->n = 0;
    return pthread_mutex_init(&s->m, NULL) || pthread_cond_init(&s->c, NULL) ? -1 : 0;
}
static void sema_wait(sema *s)
{
    pthread_mutex_lock(&s->m);
    while (s->n == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->n--;
    pthread_mutex_unlock(&s->m);
}
static void sema_post(sema *s)
{
    pthread_mutex_lock(&s->m);
    s->n++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
static int system_cores(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#define HAVE_THREADS 1
#else
#define HAVE_THREADS 0
#endif

/* --- The pool ---------------------------------------------------------- */

static int g_count;      /* 0: not started yet */
#if HAVE_THREADS
static int g_workers, g_quit;
static sema g_start[MAX_WORKERS], g_done;
static xf_job_fn g_fn;
static void *g_ctx;
static int g_parts;

/* Worker k runs parts k + 1, k + 1 + count, ... */
static void worker_loop(int k)
{
    for (;;) {
        int p;
        sema_wait(&g_start[k]);
        if (g_quit)
            break;
        for (p = k + 1; p < g_parts; p += g_count)
            g_fn(g_ctx, p, g_parts);
        sema_post(&g_done);
    }
    sema_post(&g_done);
}

#if defined(__vita__)
static SceUID g_threads[MAX_WORKERS];
static int vita_entry(SceSize args, void *argp)
{
    (void)args;
    worker_loop(*(const int *)argp);
    return 0;
}
static int start_thread(int k)
{
    /* SCE_KERNEL_CPU_MASK_USER_1 and _2: off the main thread's core */
    static const int mask[2] = { 0x20000, 0x40000 };
    g_threads[k] = sceKernelCreateThread("xfjob", vita_entry, 0x10000100, 0x10000, 0, mask[k % 2], NULL);
    return g_threads[k] >= 0 && sceKernelStartThread(g_threads[k], sizeof(int), &k) >= 0 ? 0 : -1;
}
static void join_thread(int k)
{
    sceKernelWaitThreadEnd(g_threads[k], NULL, NULL);
    sceKernelDeleteThread(g_threads[k]);
}
#elif defined(_WIN32)
static HANDLE g_threads[MAX_WORKERS];
static int g_ids[MAX_WORKERS];
static DWORD WINAPI win_entry(LPVOID arg)
{
    worker_loop(*(const int *)arg);
    return 0;
}
static int start_thread(int k)
{
    g_ids[k] = k;
    g_threads[k] = CreateThread(NULL, 0, win_entry, &g_ids[k], 0, NULL);
    return g_threads[k] ? 0 : -1;
}
static void join_thread(int k)
{
    WaitForSingleObject(g_threads[k], INFINITE);
    CloseHandle(g_threads[k]);
}
#else
static pthread_t g_threads[MAX_WORKERS];
static int g_ids[MAX_WORKERS];
static void *posix_entry(void *arg)
{
    worker_loop(*(const int *)arg);
    return NULL;
}
static int start_thread(int k)
{
    g_ids[k] = k;
    return pthread_create(&g_threads[k], NULL, posix_entry, &g_ids[k]) == 0 ? 0 : -1;
}
static void join_thread(int k) { pthread_join(g_threads[k], NULL); }
#endif
#endif

int xf_jobs_count(void)
{
    if (g_count)
        return g_count;
    g_count = 1;
#if HAVE_THREADS
    {
        const char *env = getenv("XF_THREADS");
        int want = env ? atoi(env) : system_cores(), k;
        if (want > MAX_WORKERS + 1)
            want = MAX_WORKERS + 1;
        if (want < 2 || sema_init(&g_done) != 0)
            return g_count;
        for (k = 0; k < want - 1; k++)
            if (sema_init(&g_start[k]) != 0 || start_thread(k) != 0)
                break;
        g_workers = k;
        g_count = 1 + k;
    }
#endif
    return g_count;
}

void xf_jobs_run(xf_job_fn fn, void *ctx, int parts)
{
    int p;
#if HAVE_THREADS
    int k, busy;
    if (xf_jobs_count() > 1 && parts > 1) {
        g_fn = fn;
        g_ctx = ctx;
        g_parts = parts;
        busy = g_workers < parts - 1 ? g_workers : parts - 1;
        for (k = 0; k < busy; k++)
            sema_post(&g_start[k]);
        for (p = 0; p < parts; p += g_count)
            fn(ctx, p, parts);
        for (k = 0; k < busy; k++)
            sema_wait(&g_done);
        return;
    }
#endif
    for (p = 0; p < parts; p++)
        fn(ctx, p, parts);
}

void xf_jobs_shutdown(void)
{
#if HAVE_THREADS
    int k;
    if (g_count <= 1)
        return;
    g_quit = 1;
    for (k = 0; k < g_workers; k++)
        sema_post(&g_start[k]);
    for (k = 0; k < g_workers; k++) {
        sema_wait(&g_done);
        join_thread(k);
    }
    g_workers = 0;
    g_count = 1;
    g_quit = 0;
#endif
}
