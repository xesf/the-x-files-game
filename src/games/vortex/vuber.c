/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "vuber.h"

#include <stdlib.h>
#include <string.h>

/* DS:756: the events that are logged only once. */
static const u16 g_seen_ids[169] = {
    2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018,
    2019, 2020, 2021, 2022, 2023, 2024, 2111, 2112, 2113, 2114, 2115, 2116, 2117, 2118, 2119, 2120, 2100, 2101,
    2102, 2103, 2104, 2105, 2106, 2107, 2121, 2122, 2123, 2124, 2125, 2130, 2131, 2132, 2133, 2134, 2135, 2136,
    2137, 2140, 2141, 2142, 2143, 2144, 2145, 2146, 2147, 2150, 2151, 2152, 2153, 2160, 2161, 2170, 2171, 2173,
    2172, 2174, 2175, 2200, 2201, 2202, 2203, 2204, 2205, 2206, 2207, 2208, 2209, 2210, 2211, 2212, 2213, 2214,
    2215, 2216, 2217, 2218, 2219, 2220, 2221, 2222, 2223, 2224, 2225, 2226, 2227, 2228, 2230, 2231, 2232, 2233,
    2234, 2235, 2236, 2237, 2240, 2241, 2242, 2243, 2244, 2245, 2246, 2247, 2248, 2250, 2251, 2252, 2253, 2254,
    2255, 2256, 2257, 2258, 2259, 2260, 2261, 2262, 2263, 2264, 2265, 2266, 2267, 2268, 2269, 2270, 2271, 2272,
    2273, 2274, 2275, 2276, 2277, 2278, 2279, 2280, 2281, 2282, 2283, 2284, 2285, 2286, 2287, 2288, 2289, 2290,
    2291, 2292, 2293, 2294, 2295, 2296, 2297,
};

/* seg13:10C2: the events every game starts with: mood, id, then the
 * seven words (unlisted ones: none). */
static const u16 g_seed[24][9] = {
    { 1, 2001, VU_NONE, VU_NONE, VU_NONE, 120, 40, 50, 39 },
    { 1, 2002, 2602, VU_NONE, 2402, 60, 220, 180, 140 },
    { 1, 2003, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2004, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2005, 2605, VU_NONE, 2405, 35, 200, 240, 180 },
    { 0, 2006, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2007, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2008, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2009, VU_NONE, VU_NONE, VU_NONE, 120, 280, 60, 50 },
    { 0, 2010, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2011, 2611, VU_NONE, 2411, 30, 180, 240, 180 },
    { 0, 2012, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 0, 2013, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2014, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2015, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2016, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2017, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 1, 2018, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 2, 2019, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 2, 2020, 2620, VU_NONE, 2420, 45, 170, 200, 200 },
    { 2, 2021, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 2, 2022, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 2, 2023, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
    { 2, 2024, VU_NONE, VU_NONE, VU_NONE, 0, 0, 0, 0 },
};

void vu_init(vu_state *u)
{
    memset(u, 0, sizeof(*u));
    u->page = -1;
}

void vu_free(vu_state *u)
{
    int m;
    for (m = 0; m < VU_MOODS; m++) {
        free(u->mood[m].values);
        u->mood[m].values = NULL;
    }
    free(u->events);
    u->events = NULL;
    free(u->book);
    u->book = NULL;
    u->ready = 0;
}

/* seg13:0000: a no-op when ready; the cursor, the page and the seen
 * flags are kept or cleared as the original does. */
static void start(vu_state *u)
{
    int m;
    if (u->ready)
        return;
    for (m = 0; m < VU_MOODS; m++) {
        free(u->mood[m].values);
        memset(&u->mood[m], 0, sizeof(u->mood[m]));
    }
    free(u->events);
    u->events = NULL;
    u->event_count = u->event_cap = 0;
    u->def_mood = 0;
    memset(u->seen, 0, sizeof(u->seen));
    u->page = -1;
    u->ready = 1;
}

int vu_seed(vu_state *u)
{
    int i;
    start(u);
    for (i = 0; i < 24; i++)
        vu_record(u, g_seed[i][0], 0, g_seed[i][1], &g_seed[i][2]);
    return 1;
}

static int seen_index(u16 id)
{
    int i;
    for (i = 0; i < 169; i++)
        if (g_seen_ids[i] == id)
            return i;
    return -1;
}

int vu_record(vu_state *u, int m, u8 v, u16 id, const u16 *w)
{
    if (!u->ready)
        vu_seed(u);
    if (m >= 0 && m < VU_MOODS) {
        if (u->mood[m].count == u->mood[m].cap) {
            int cap = u->mood[m].cap + 10;
            u8 *p = realloc(u->mood[m].values, (size_t)cap);
            if (!p)
                return 0;
            u->mood[m].values = p;
            u->mood[m].cap = cap;
        }
        u->mood[m].values[u->mood[m].count++] = v;
        u->mood[m].total += v;
        if (v)
            u->mood[m].positives++;
    }
    if (id == VU_NONE && w[0] == VU_NONE)
        return 1;
    {
        int s = seen_index(id);
        vu_event *e;
        if (s >= 0) {
            if (u->seen[s])
                return 1; /* logged before */
            u->seen[s] = 1;
        }
        if (u->event_count == u->event_cap) {
            int cap = u->event_cap + 10;
            vu_event *p = realloc(u->events, sizeof(*p) * (size_t)cap);
            if (!p)
                return 0;
            u->events = p;
            u->event_cap = cap;
        }
        e = &u->events[u->event_count++];
        e->mood = (u16)(m == 0xFF ? vu_dominant(u, 1, 0) : m);
        e->id = id;
        memcpy(e->w, w, sizeof(e->w));
    }
    return 1;
}

/* A mood's sum by a mode (seg13:05EA, 089E); `nonzero` gets how many
 * values counted were above 0. */
static double sum_of(const vu_state *u, int i, int mode, int n, int *nonzero)
{
    double x = 0;
    int k, j;
    *nonzero = 0;
    switch (mode) {
    case 1:
        if (u->mood[i].count)
            x = u->mood[i].values[u->mood[i].count - 1];
        break;
    case 2:
        for (k = u->mood[i].count - 1, j = 0; k >= 0 && j < n; k--, j++) {
            x += u->mood[i].values[k];
            if (u->mood[i].values[k])
                ++*nonzero;
        }
        break;
    case 3:
        x = u->mood[i].total;
        *nonzero = u->mood[i].positives;
        break;
    case 4:
        for (k = u->mood[i].count - 1, j = 1; k >= 0; k--, j++) {
            x += j > n ? u->mood[i].values[k] : 2 * u->mood[i].values[k];
            if (u->mood[i].values[k])
                ++*nonzero;
        }
        break;
    }
    return x;
}

int vu_share(const vu_state *u, int m, int mode, int n, float tol)
{
    double all = 0, mine = 0;
    int i, nz;
    for (i = 0; i < VU_MOODS; i++) {
        double x = sum_of(u, i, mode, n, &nz);
        all += x;
        if (i == m)
            mine += x;
    }
    if (all == 0.0)
        return 0;
    return mine / all >= tol;
}

int vu_dominant(const vu_state *u, int mode, int n)
{
    double sums[VU_MOODS], best_val = 0;
    int cnt[VU_MOODS], best = u->def_mood, i;
    if (mode < 1 || mode > 4)
        return best;
    for (i = 0; i < VU_MOODS; i++) {
        sums[i] = sum_of(u, i, mode, n, &cnt[i]);
        if (mode == 4) { /* the totals are counted once more */
            sums[i] += u->mood[i].total;
            cnt[i] += u->mood[i].positives;
        }
        if (sums[i] > best_val) {
            best = i;
            best_val = sums[i];
        }
    }
    if (mode != 1 && best < VU_MOODS) /* a tie goes to the one with more values */
        for (i = 0; i < VU_MOODS; i++)
            if (i != best && sums[i] == sums[best] && cnt[best] < cnt[i])
                best = i;
    return best;
}

int vu_balanced(const vu_state *u, int m1, int m2, float tol)
{
    double diff, sum;
    if (m1 < 0 || m1 >= VU_MOODS || m2 < 0 || m2 >= VU_MOODS)
        return 0;
    diff = (double)(i32)(u->mood[m1].total - u->mood[m2].total);
    if (diff < 0)
        diff = -diff;
    sum = (double)u->mood[0].total + (double)u->mood[1].total + (double)u->mood[2].total;
    if (sum == 0)
        return 0;
    return diff / sum <= tol;
}

u32 vu_colour(const vu_state *u, int mood, int level)
{
    static const u32 c[4][4] = {
        { 0x006000, 0x32E032, 0x000400, 0x206020 }, /* mood 0: green */
        { 0x802080, 0xA040A0, 0x020002, 0xC00040 }, /* 1 and 2 even: purple */
        { 0xA02020, 0xE03232, 0x040000, 0xA40000 }, /* 2: red */
        { 0x202080, 0x3232E0, 0x000004, 0x202080 }, /* 1: blue */
    };
    int set;
    if (level < 0 || level > 3)
        return 0;
    if (mood == 0)
        set = 0;
    else if (vu_balanced(u, 2, 1, 0.2f))
        set = 1;
    else if (mood == 2)
        set = 2;
    else if (mood == 1)
        set = 3;
    else
        return 0;
    return c[set][level];
}

int vu_move(vu_state *u, int mode)
{
    int last = u->event_count - 1;
    switch (mode) {
    case 1: u->cursor = 0; return 0;
    case 2: u->cursor = last; return last;
    case 3: return u->cursor + 1 <= last ? ++u->cursor : -1;
    case 4: return u->cursor - 1 >= 0 ? --u->cursor : -1;
    }
    return -1;
}

void vu_mark_seen(vu_state *u, u16 id)
{
    int s = seen_index(id);
    if (s >= 0)
        u->seen[s] = 1;
}

int vu_open(vu_state *u)
{
    int n = u->event_count, i;
    u->turned = 0;
    u->book_count = n;
    u->book_pages = n / 2 + 2;
    free(u->book);
    if (!(u->book = malloc(sizeof(int) * 2 * (size_t)u->book_pages)))
        return -2;
    /* page 0: nothing, then the first event; page p: events 2p-1, 2p */
    u->book[0] = -1;
    for (i = 1; i < 2 * u->book_pages; i++)
        u->book[i] = vu_move(u, i == 1 ? 1 : i == n ? 2 : 3);
    return u->page;
}

void vu_close(vu_state *u)
{
    free(u->book);
    u->book = NULL;
    if (u->turned && u->page > -1) {
        u->page--;
        if (u->cursor - 1 >= 0)
            u->cursor--;
    }
}

static void side(const vu_state *u, int index, vu_event *e)
{
    int k;
    if (u->page >= 0 && u->page < u->book_pages && u->book && index > -1 && index < u->event_count) {
        *e = u->events[index];
        return;
    }
    e->mood = 0xFF;
    e->id = VU_NONE;
    for (k = 0; k < 3; k++)
        e->w[k] = VU_NONE;
    for (k = 3; k < 7; k++)
        e->w[k] = 0;
}

void vu_turn(vu_state *u, int dir, vu_event *left, vu_event *right)
{
    int ok;
    u->turned = 1;
    if (dir == 1 && u->page > -1)
        u->page--;
    else if (dir == 2 && u->event_count > u->page)
        u->page++;
    ok = u->page >= 0 && u->page < u->book_pages && u->book;
    side(u, ok ? u->book[2 * u->page] : -1, left);
    side(u, ok ? u->book[2 * u->page + 1] : -1, right);
}
