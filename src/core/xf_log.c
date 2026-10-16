/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "xf_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int g_level = XF_LOG_INFO;
static FILE *g_file; /* NULL: stderr */
static const char *const g_tags[] = { "debug", "info", "warn", "error" };

void xf_log_set_level(int level) { g_level = level; }

void xf_log_set_file(void *file) { g_file = file; }

void xf_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (level < g_level)
        return;
    FILE *out = g_file ? g_file : stderr;
    fprintf(out, "[%s] ", g_tags[level]);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    if (g_file)
        fflush(g_file);
}

void xf_fatal(const char *fmt, ...)
{
    va_list ap;
    fputs("[fatal] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
