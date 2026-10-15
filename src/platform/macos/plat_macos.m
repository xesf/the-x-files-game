/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* macOS backend: Cocoa window, Core Graphics blit, Audio Queue output. */
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "platform/plat.h"

#define EVENT_QUEUE_SIZE 256
#define AUDIO_BUFFERS 3
#define AUDIO_FRAMES 1024

static NSWindow *g_window;
static int g_width, g_height;
static CGImageRef g_frame;
static plat_event g_events[EVENT_QUEUE_SIZE];
static int g_ev_head, g_ev_tail;
static mach_timebase_info_data_t g_timebase;
static u64 g_start;

static AudioQueueRef g_queue;
static plat_audio_fn g_audio_fn;
static void *g_audio_user;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static void push_event(plat_event ev)
{
    int next = (g_ev_head + 1) % EVENT_QUEUE_SIZE;
    if (next == g_ev_tail)
        return;
    g_events[g_ev_head] = ev;
    g_ev_head = next;
}

static int map_key(NSEvent *e)
{
    NSString *s;
    switch (e.keyCode) {
    case 53: return PLAT_KEY_ESCAPE;
    case 36: return PLAT_KEY_RETURN;
    case 49: return PLAT_KEY_SPACE;
    case 123: return PLAT_KEY_LEFT;
    case 124: return PLAT_KEY_RIGHT;
    case 126: return PLAT_KEY_UP;
    case 125: return PLAT_KEY_DOWN;
    case 122: return PLAT_KEY_F1;
    case 51: return 8; /* backspace */
    }
    s = e.charactersIgnoringModifiers;
    if (s.length == 1 && [s characterAtIndex:0] < 128) {
        int c = (int)[s characterAtIndex:0];
        return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    }
    return 0;
}

@interface XFView : NSView
@end

@implementation XFView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty
{
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetFillColorWithColor(ctx, CGColorGetConstantColor(kCGColorBlack));
    CGContextFillRect(ctx, self.bounds);
    if (!g_frame)
        return;
    /* Keep the 4:3 picture centred. */
    NSRect b = self.bounds;
    CGFloat scale = MIN(b.size.width / g_width, b.size.height / g_height);
    CGFloat w = g_width * scale, h = g_height * scale;
    CGRect r = CGRectMake((b.size.width - w) / 2, (b.size.height - h) / 2, w, h);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, b.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    r.origin.y = b.size.height - r.origin.y - h;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, r, g_frame);
    CGContextRestoreGState(ctx);
}

- (NSPoint)framePoint:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    CGFloat scale = MIN(b.size.width / g_width, b.size.height / g_height);
    p.x = (p.x - (b.size.width - g_width * scale) / 2) / scale;
    p.y = (p.y - (b.size.height - g_height * scale) / 2) / scale;
    return p;
}

- (void)mouseEvent:(NSEvent *)e type:(int)type button:(int)button
{
    NSPoint p = [self framePoint:e];
    plat_event ev = { type, (int)p.x, (int)p.y, button, 0, 0 };
    push_event(ev);
}

- (void)mouseMoved:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_MOVE button:0]; }
- (void)mouseDragged:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_MOVE button:0]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_MOVE button:0]; }
- (void)mouseDown:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_DOWN button:PLAT_MOUSE_LEFT]; }
- (void)mouseUp:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_UP button:PLAT_MOUSE_LEFT]; }
- (void)rightMouseDown:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_DOWN button:PLAT_MOUSE_RIGHT]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouseEvent:e type:PLAT_EV_MOUSE_UP button:PLAT_MOUSE_RIGHT]; }

static int map_mods(NSEvent *e)
{
    NSEventModifierFlags f = e.modifierFlags;
    return (f & NSEventModifierFlagShift ? PLAT_MOD_SHIFT : 0) | (f & NSEventModifierFlagControl ? PLAT_MOD_CTRL : 0) |
           (f & NSEventModifierFlagOption ? PLAT_MOD_ALT : 0) | (f & NSEventModifierFlagCommand ? PLAT_MOD_CMD : 0);
}

- (void)keyDown:(NSEvent *)e
{
    plat_event ev = { PLAT_EV_KEY_DOWN, 0, 0, 0, map_key(e), map_mods(e) };
    if (ev.key && !e.isARepeat)
        push_event(ev);
}

- (void)keyUp:(NSEvent *)e
{
    plat_event ev = { PLAT_EV_KEY_UP, 0, 0, 0, map_key(e), map_mods(e) };
    if (ev.key)
        push_event(ev);
}
@end

@interface XFWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation XFWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)w
{
    plat_event ev = { PLAT_EV_QUIT, 0, 0, 0, 0, 0 };
    push_event(ev);
    return NO;
}
@end

static XFWindowDelegate *g_delegate;
static int g_headless;

int plat_init(const char *title, int width, int height)
{
    NSRect rect = NSMakeRect(0, 0, width * 2, height * 2);
    XFView *view;

    g_width = width;
    g_height = height;
    mach_timebase_info(&g_timebase);
    g_start = mach_absolute_time();
    /* XF_HEADLESS: no window and no sound, for scripted test runs. */
    g_headless = getenv("XF_HEADLESS") != NULL;
    if (g_headless)
        return 0;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    g_window = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    g_window.title = [NSString stringWithUTF8String:title];
    g_window.contentMinSize = NSMakeSize(width, height);
    g_window.acceptsMouseMovedEvents = YES;
    g_delegate = [XFWindowDelegate new];
    g_window.delegate = g_delegate;
    view = [[XFView alloc] initWithFrame:rect];
    g_window.contentView = view;
    [g_window makeFirstResponder:view];
    [g_window center];
    [g_window makeKeyAndOrderFront:nil];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
    if (getenv("XF_PRINT_WINDOW_ID"))
        { printf("window-id %ld\n", (long)g_window.windowNumber); fflush(stdout); }
    return 0;
}

void plat_shutdown(void)
{
    plat_audio_close();
    if (g_frame)
        CGImageRelease(g_frame);
    g_frame = NULL;
    [g_window close];
    g_window = nil;
}

int plat_poll(plat_event *ev)
{
    if (g_headless)
        return 0;
    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:nil
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES]) != nil)
            [NSApp sendEvent:e];
    }
    if (g_ev_tail == g_ev_head)
        return 0;
    *ev = g_events[g_ev_tail];
    g_ev_tail = (g_ev_tail + 1) % EVENT_QUEUE_SIZE;
    return 1;
}

void plat_present(const u32 *pixels, int width, int height, int pitch)
{
    if (g_headless)
        return;
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)pitch * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        if (g_frame)
            CGImageRelease(g_frame);
        g_frame = CGImageCreate(width, height, 8, 32, pitch * 4, cs,
                                kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                provider, NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(cs);
        g_window.contentView.needsDisplay = YES;
        [g_window.contentView displayIfNeeded];
    }
}

u32 plat_ticks_ms(void)
{
    u64 t = mach_absolute_time() - g_start;
    return (u32)(t * g_timebase.numer / g_timebase.denom / 1000000);
}

void plat_sleep_ms(u32 ms) { usleep(ms * 1000); }

static void audio_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    int frames = (int)(buf->mAudioDataBytesCapacity / 4);
    pthread_mutex_lock(&g_audio_mutex);
    if (g_audio_fn)
        g_audio_fn(g_audio_user, (i16 *)buf->mAudioData, frames);
    else
        memset(buf->mAudioData, 0, buf->mAudioDataBytesCapacity);
    pthread_mutex_unlock(&g_audio_mutex);
    buf->mAudioDataByteSize = (UInt32)frames * 4;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

int plat_audio_open(int rate, plat_audio_fn fn, void *user)
{
    AudioStreamBasicDescription fmt = { 0 };
    int i;

    if (g_headless)
        return -1;
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = 4;

    g_audio_fn = fn;
    g_audio_user = user;
    if (AudioQueueNewOutput(&fmt, audio_callback, NULL, NULL, NULL, 0, &g_queue) != noErr)
        return -1;
    for (i = 0; i < AUDIO_BUFFERS; i++) {
        AudioQueueBufferRef buf;
        AudioQueueAllocateBuffer(g_queue, AUDIO_FRAMES * 4, &buf);
        buf->mAudioDataByteSize = AUDIO_FRAMES * 4;
        audio_callback(NULL, g_queue, buf);
    }
    return AudioQueueStart(g_queue, NULL) == noErr ? 0 : -1;
}

void plat_audio_close(void)
{
    if (!g_queue)
        return;
    AudioQueueStop(g_queue, true);
    AudioQueueDispose(g_queue, true);
    g_queue = NULL;
}

void plat_audio_lock(void) { pthread_mutex_lock(&g_audio_mutex); }
void plat_audio_unlock(void) { pthread_mutex_unlock(&g_audio_mutex); }

int plat_font_path(const char *family, char *out, size_t size)
{
    static const char *const dirs[] = { "/System/Library/Fonts/Supplemental", "/System/Library/Fonts",
                                        "/Library/Fonts" };
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, size, "%s/%s.ttf", dirs[i], family);
        if (access(out, R_OK) == 0)
            return 0;
    }
    return -1;
}

void plat_show_cursor(int show)
{
    static int hidden;
    if (g_headless || hidden == !show)
        return;
    if (show)
        [NSCursor unhide];
    else
        [NSCursor hide];
    hidden = !show;
}

/* plat_save_dir: see plat.h. */
int plat_save_dir(char *out, size_t size)
{
    const char *home = getenv("HOME");
    int n;
    if (!home)
        return -1;
    n = snprintf(out, size, "%s/Library/Application Support/The X-Files", home);
    if (n < 0 || (size_t)n >= size)
        return -1;
    mkdir(out, 0755);
    return 0;
}
