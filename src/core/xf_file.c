/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "xf_file.h"

#ifdef __WATCOMC__
#include <direct.h> /* opendir and readdir */
#else
#include <dirent.h>
#include <strings.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct xf_file {
    FILE *fp;
    u64 size;
};

/* Appends one path component to `out`, matching it without case. */
static int resolve_component(char *out, size_t out_size, const char *name, size_t len)
{
    size_t base = strlen(out);
    struct stat st;
    DIR *dir;
    struct dirent *de;

    if (base + 1 + len + 1 > out_size)
        return -1;
    if (base > 0 && out[base - 1] != '/')
        out[base++] = '/';
    memcpy(out + base, name, len);
    out[base + len] = '\0';
    if (stat(out, &st) == 0)
        return 0;

    out[base] = '\0';
    dir = opendir(base ? out : ".");
    if (!dir)
        return -1;
    while ((de = readdir(dir)) != NULL) {
        if (strlen(de->d_name) == len && strncasecmp(de->d_name, name, len) == 0) {
            memcpy(out + base, de->d_name, len + 1);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

int xf_path_resolve(const char *root, const char *rel, char *out, size_t out_size)
{
    const char *p = rel;

    if (strlen(root) + 1 > out_size)
        return -1;
    strcpy(out, root);
    while (*p) {
        size_t len = strcspn(p, "/\\");
        if (len > 0 && resolve_component(out, out_size, p, len) != 0)
            return -1;
        p += len;
        if (*p)
            p++;
    }
    return 0;
}

xf_file *xf_open(const char *path)
{
    xf_file *f;
    FILE *fp = fopen(path, "rb");

    if (!fp)
        return NULL;
    f = calloc(1, sizeof(*f));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    fseeko(fp, 0, SEEK_END);
    f->size = (u64)ftello(fp);
    fseeko(fp, 0, SEEK_SET);
    return f;
}

void xf_close(xf_file *f)
{
    if (!f)
        return;
    fclose(f->fp);
    free(f);
}

u64 xf_size(xf_file *f) { return f->size; }

int xf_seek(xf_file *f, u64 pos) { return fseeko(f->fp, (off_t)pos, SEEK_SET) == 0 ? 0 : -1; }

u64 xf_tell(xf_file *f) { return (u64)ftello(f->fp); }

int xf_read(xf_file *f, void *dst, size_t n) { return fread(dst, 1, n, f->fp) == n ? 0 : -1; }

int xf_read_at(xf_file *f, u64 pos, void *dst, size_t n)
{
    if (xf_seek(f, pos) != 0)
        return -1;
    return xf_read(f, dst, n);
}

u8 *xf_load(const char *path, size_t *size)
{
    xf_file *f = xf_open(path);
    u8 *buf;

    if (!f)
        return NULL;
    buf = malloc(f->size ? (size_t)f->size : 1);
    if (!buf || xf_read(f, buf, (size_t)f->size) != 0) {
        free(buf);
        xf_close(f);
        return NULL;
    }
    if (size)
        *size = (size_t)f->size;
    xf_close(f);
    return buf;
}
