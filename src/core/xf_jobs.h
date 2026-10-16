/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Spreads work over the CPU cores: a pool of worker threads, one per
 * extra core (Vita kernel threads on cores 1 and 2, pthreads, Win32
 * threads), or one part after another where there are no threads (DOS). */
#ifndef XF_JOBS_H
#define XF_JOBS_H

#include "xf_types.h"

/* One part of a job: `part` from 0 to `parts` - 1. */
typedef void (*xf_job_fn)(void *ctx, int part, int parts);

/* The number of parts worth splitting a job into: the cores in use.
 * Starts the workers on first use; XF_THREADS=n sets the count. */
int xf_jobs_count(void);
/* Runs fn for every part, spread over the cores, and returns when all
 * have run. The caller runs part 0 itself. Not reentrant: a part must
 * not run another job. */
void xf_jobs_run(xf_job_fn fn, void *ctx, int parts);
/* Stops the workers. */
void xf_jobs_shutdown(void);

#endif
