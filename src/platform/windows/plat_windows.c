/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Windows backend: a Win32 window drawn with GDI (StretchDIBits) and
 * waveOut sound fed by a thread. Only ANSI Win32 calls that Windows 95
 * already had, so the same file builds for Windows 9x. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/plat.h"

#define EVENT_QUEUE_SIZE 256

static HWND g_hwnd;
static int g_width, g_height; /* the last frame shown */
static int g_init_w, g_init_h;
static const u32 *g_pixels;
static int g_pitch;
static BITMAPINFO g_bmi;
static plat_event g_events[EVENT_QUEUE_SIZE];
static int g_ev_head, g_ev_tail;
static DWORD g_start;
static int g_headless;
static int g_cursor_hidden;

static void push_event(plat_event ev)
{
    int next = (g_ev_head + 1) % EVENT_QUEUE_SIZE;
    if (next == g_ev_tail)
        return;
    g_events[g_ev_head] = ev;
    g_ev_head = next;
}

/* The 4:3 picture inside the client area. */
static void view_rect(RECT *r)
{
    RECT c;
    int w, h;
    GetClientRect(g_hwnd, &c);
    w = c.right;
    h = c.right * g_height / g_width;
    if (h > c.bottom) {
        h = c.bottom;
        w = c.bottom * g_width / g_height;
    }
    r->left = (c.right - w) / 2;
    r->top = (c.bottom - h) / 2;
    r->right = r->left + w;
    r->bottom = r->top + h;
}

static void to_frame(LPARAM lp, int *x, int *y)
{
    RECT r;
    int w, h;
    view_rect(&r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    *x = w ? ((short)LOWORD(lp) - r.left) * g_width / w : 0;
    *y = h ? ((short)HIWORD(lp) - r.top) * g_height / h : 0;
}

static int mods_now(void)
{
    return (GetKeyState(VK_SHIFT) < 0 ? PLAT_MOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? PLAT_MOD_CTRL : 0) |
           (GetKeyState(VK_MENU) < 0 ? PLAT_MOD_ALT : 0);
}

static int map_key(WPARAM vk)
{
    switch (vk) {
    case VK_ESCAPE: return PLAT_KEY_ESCAPE;
    case VK_RETURN: return PLAT_KEY_RETURN;
    case VK_SPACE: return PLAT_KEY_SPACE;
    case VK_LEFT: return PLAT_KEY_LEFT;
    case VK_RIGHT: return PLAT_KEY_RIGHT;
    case VK_UP: return PLAT_KEY_UP;
    case VK_DOWN: return PLAT_KEY_DOWN;
    case VK_F1: return PLAT_KEY_F1;
    case VK_BACK: return 8;
    }
    if (vk >= 'A' && vk <= 'Z')
        return (int)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9')
        return (int)vk;
    {
        /* Punctuation: the character the key gives without Shift. */
        UINT c = MapVirtualKeyA((UINT)vk, 2 /* MAPVK_VK_TO_CHAR */) & 0x7FFF;
        return c >= 32 && c < 127 ? (int)c : 0;
    }
}

static void mouse_event_at(int type, int button, LPARAM lp)
{
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.button = button;
    to_frame(lp, &ev.x, &ev.y);
    push_event(ev);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    plat_event ev;
    memset(&ev, 0, sizeof(ev));
    switch (msg) {
    case WM_CLOSE:
        ev.type = PLAT_EV_QUIT;
        push_event(ev);
        return 0;
    case WM_MOUSEMOVE: mouse_event_at(PLAT_EV_MOUSE_MOVE, 0, lp); return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); mouse_event_at(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_LEFT, lp); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); mouse_event_at(PLAT_EV_MOUSE_UP, PLAT_MOUSE_LEFT, lp); return 0;
    case WM_RBUTTONDOWN: mouse_event_at(PLAT_EV_MOUSE_DOWN, PLAT_MOUSE_RIGHT, lp); return 0;
    case WM_RBUTTONUP: mouse_event_at(PLAT_EV_MOUSE_UP, PLAT_MOUSE_RIGHT, lp); return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (lp & (1L << 30)) /* auto-repeat */
            return 0;
        /* fall through */
    case WM_KEYUP:
    case WM_SYSKEYUP:
        ev.type = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ? PLAT_EV_KEY_DOWN : PLAT_EV_KEY_UP;
        ev.key = map_key(wp);
        ev.mods = mods_now();
        if (ev.key)
            push_event(ev);
        if (msg == WM_SYSKEYDOWN && wp == VK_F4)
            break; /* Alt+F4 closes */
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_cursor_hidden) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c, r;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        GetClientRect(hwnd, &c);
        view_rect(&r);
        /* Black bars around the picture. */
        {
            RECT bar;
            SetRect(&bar, 0, 0, c.right, r.top);
            FillRect(dc, &bar, black);
            SetRect(&bar, 0, r.bottom, c.right, c.bottom);
            FillRect(dc, &bar, black);
            SetRect(&bar, 0, r.top, r.left, r.bottom);
            FillRect(dc, &bar, black);
            SetRect(&bar, r.right, r.top, c.right, r.bottom);
            FillRect(dc, &bar, black);
        }
        if (g_pixels) {
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, g_width, g_height,
                          g_pixels, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int plat_init(const char *title, int width, int height)
{
    WNDCLASSA wc;
    RECT r;
    DWORD style = WS_OVERLAPPEDWINDOW;

    g_width = g_init_w = width;
    g_height = g_init_h = height;
    g_start = GetTickCount();
    g_headless = getenv("XF_HEADLESS") != NULL;
    if (g_headless)
        return 0;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "XFilesWindow";
    if (!RegisterClassA(&wc))
        return -1;
    SetRect(&r, 0, 0, width * 2, height * 2);
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd)
        return -1;
    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = width;
    g_bmi.bmiHeader.biHeight = -height; /* top-down */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return 0;
}

void plat_shutdown(void)
{
    plat_audio_close();
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    g_hwnd = NULL;
}

int plat_poll(plat_event *ev)
{
    MSG msg;
    if (g_headless)
        return 0;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_ev_tail == g_ev_head)
        return 0;
    *ev = g_events[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % EVENT_QUEUE_SIZE;
    return 1;
}


/* XF_OUTPUT=<w>x<h>: the output size without a window, for tests. */
static int headless_output(int *width, int *height)
{
    const char *s = getenv("XF_OUTPUT");
    int w, h;
    if (!s || sscanf(s, "%dx%d", &w, &h) != 2 || w < 64 || h < 64)
        return 0;
    *width = w;
    *height = h;
    return 1;
}

void plat_output_size(int *width, int *height)
{
    RECT c;
    *width = g_init_w;
    *height = g_init_h;
    if (g_headless || !g_hwnd) {
        headless_output(width, height);
        return;
    }
    GetClientRect(g_hwnd, &c);
    if (c.right >= 64 && c.bottom >= 64) {
        *width = c.right;
        *height = c.bottom;
    }
}

void plat_present(const u32 *pixels, int width, int height, int pitch)
{
    if (g_headless || !g_hwnd)
        return;
    g_pixels = pixels;
    g_pitch = pitch;
    g_width = width;
    g_height = height;
    g_bmi.bmiHeader.biWidth = pitch;
    g_bmi.bmiHeader.biHeight = -height;
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
}

u32 plat_ticks_ms(void) { return (u32)(GetTickCount() - g_start); }

void plat_sleep_ms(u32 ms) { Sleep(ms); }

/* --- Sound ---------------------------------------------------------- */

#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 1024

static HWAVEOUT g_wave;
static WAVEHDR g_hdr[AUDIO_BUFFERS];
static i16 g_buf[AUDIO_BUFFERS][AUDIO_FRAMES * 2];
static HANDLE g_audio_thread, g_audio_event;
static CRITICAL_SECTION g_audio_cs;
static int g_audio_cs_ready;
static plat_audio_fn g_audio_fn;
static void *g_audio_user;
static volatile LONG g_audio_run;

static void fill(int i)
{
    EnterCriticalSection(&g_audio_cs);
    g_audio_fn(g_audio_user, g_buf[i], AUDIO_FRAMES);
    LeaveCriticalSection(&g_audio_cs);
    waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
}

/* waveOut signals the event when a buffer is done; the thread refills it. */
static DWORD WINAPI audio_main(LPVOID arg)
{
    (void)arg;
    while (g_audio_run) {
        int i;
        WaitForSingleObject(g_audio_event, 100);
        for (i = 0; i < AUDIO_BUFFERS && g_audio_run; i++)
            if (g_hdr[i].dwFlags & WHDR_DONE)
                fill(i);
    }
    return 0;
}

int plat_audio_open(int rate, plat_audio_fn fn, void *user)
{
    WAVEFORMATEX fmt;
    DWORD id;
    int i;
    if (g_headless)
        return -1;
    if (!g_audio_cs_ready) {
        InitializeCriticalSection(&g_audio_cs);
        g_audio_cs_ready = 1;
    }
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = (DWORD)rate * 4;
    g_audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, (DWORD_PTR)g_audio_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(g_audio_event);
        return -1;
    }
    g_audio_fn = fn;
    g_audio_user = user;
    for (i = 0; i < AUDIO_BUFFERS; i++) {
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = AUDIO_FRAMES * 4;
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        fill(i);
    }
    g_audio_run = 1;
    g_audio_thread = CreateThread(NULL, 0, audio_main, NULL, 0, &id);
    return g_audio_thread ? 0 : -1;
}

void plat_audio_close(void)
{
    int i;
    if (!g_wave)
        return;
    g_audio_run = 0;
    if (g_audio_thread) {
        SetEvent(g_audio_event);
        WaitForSingleObject(g_audio_thread, INFINITE);
        CloseHandle(g_audio_thread);
        g_audio_thread = NULL;
    }
    waveOutReset(g_wave);
    for (i = 0; i < AUDIO_BUFFERS; i++)
        waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    CloseHandle(g_audio_event);
    g_wave = NULL;
}

void plat_audio_lock(void)
{
    if (g_audio_cs_ready)
        EnterCriticalSection(&g_audio_cs);
}

void plat_audio_unlock(void)
{
    if (g_audio_cs_ready)
        LeaveCriticalSection(&g_audio_cs);
}

/* --- Fonts and cursor --------------------------------------------------- */

int plat_font_path(const char *family, char *out, size_t size)
{
    static const struct { const char *family, *file; } files[] = {
        { "Courier New", "cour.ttf" }, { "Arial", "arial.ttf" }, { "Times New Roman", "times.ttf" },
        { "Japanese", "msgothic.ttc" }, { "Japanese", "meiryo.ttc" }, { "Japanese", "YuGothR.ttc" } };
    char dir[MAX_PATH];
    size_t i;
    UINT n = GetWindowsDirectoryA(dir, sizeof(dir));
    if (!n || n >= sizeof(dir))
        return -1;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (strcmp(family, files[i].family) == 0) {
            snprintf(out, size, "%s\\Fonts\\%s", dir, files[i].file);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
                return 0;
        }
    return -1;
}

/* The mouse is the user's: navigation does not move it. */
int plat_text_input(const char *title, const char *initial, int password, int clear)
{
    (void)title;
    (void)initial;
    (void)password;
    (void)clear;
    return 0; /* a physical keyboard types into the field */
}

void plat_set_pointer(int x, int y)
{
    (void)x;
    (void)y;
}

void plat_show_cursor(int show)
{
    if (g_headless || g_cursor_hidden == !show)
        return;
    g_cursor_hidden = !show;
    if (g_hwnd) {
        POINT p;
        RECT c;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        GetClientRect(g_hwnd, &c);
        if (PtInRect(&c, p))
            SetCursor(show ? LoadCursor(NULL, IDC_ARROW) : NULL);
    }
}

/* plat_save_dir: see plat.h. */
int plat_save_dir(char *out, size_t size)
{
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", base, sizeof(base));
    int k;
    if (n == 0 || n >= sizeof(base)) /* Windows 95 has no APPDATA: next to the game */
        snprintf(base, sizeof(base), ".");
    k = snprintf(out, size, "%s\\The X-Files", base);
    if (k < 0 || (size_t)k >= size)
        return -1;
    CreateDirectoryA(out, NULL);
    return 0;
}

/* --- MIDI --------------------------------------------------------------- */

/* The MIDI mapper: the system's General MIDI synthesizer. */
static HMIDIOUT g_midi;

int plat_midi_open(void)
{
    if (g_midi)
        return 0;
    if (getenv("XF_HEADLESS") || midiOutOpen(&g_midi, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_midi = NULL;
        return -1;
    }
    return 0;
}

void plat_midi_send(u8 status, u8 d1, u8 d2)
{
    if (g_midi)
        midiOutShortMsg(g_midi, (DWORD)status | (DWORD)d1 << 8 | (DWORD)d2 << 16);
}

void plat_midi_close(void)
{
    if (g_midi) {
        midiOutReset(g_midi);
        midiOutClose(g_midi);
        g_midi = NULL;
    }
}
