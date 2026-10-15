/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Logging to stderr. */
#ifndef XF_LOG_H
#define XF_LOG_H

enum { XF_LOG_DEBUG, XF_LOG_INFO, XF_LOG_WARN, XF_LOG_ERROR };

void xf_log_set_level(int level);
void xf_log(int level, const char *fmt, ...);
void xf_fatal(const char *fmt, ...);

#define xf_debug(...) xf_log(XF_LOG_DEBUG, __VA_ARGS__)
#define xf_info(...) xf_log(XF_LOG_INFO, __VA_ARGS__)
#define xf_warn(...) xf_log(XF_LOG_WARN, __VA_ARGS__)
#define xf_error(...) xf_log(XF_LOG_ERROR, __VA_ARGS__)

#endif
