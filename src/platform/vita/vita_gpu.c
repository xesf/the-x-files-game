/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf
 * The texture shaders and the order of the sceGxm set-up follow libvita2d,
 * Copyright (c) 2015 Sergi Granell (xerpi) (MIT licence). */
#include "vita_gpu.h"

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* libvita2d's texture_v.cg and texture_f.cg, compiled:
 *   vertex:   vPosition = mul(float4(aPosition, 1), wvp); vTexcoord = aTexcoord
 *   fragment: return tex2D(tex, vTexcoord) */
static const unsigned char g_texture_v[344] __attribute__((aligned(16))) = {
    0x47, 0x58, 0x50, 0x00, 0x01, 0x04, 0x00, 0x00, 0x58, 0x01, 0x00, 0x00, 0xac, 0x24, 0x8e, 0xfa,
    0x68, 0x26, 0x9b, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x44, 0xfa,
    0x80, 0x00, 0x08, 0x83, 0x21, 0x0d, 0x80, 0x38, 0x02, 0x80, 0x81, 0xaf, 0x9c, 0x0d, 0xc0, 0x40,
    0x0e, 0x86, 0xb9, 0xff, 0xbc, 0x0d, 0xc0, 0x40, 0x00, 0x11, 0x41, 0xcf, 0x80, 0x8f, 0xb1, 0x18,
    0x02, 0x11, 0x45, 0xcf, 0x80, 0x8f, 0xb1, 0x18, 0x04, 0x11, 0x09, 0xc0, 0x81, 0x81, 0xb1, 0x18,
    0x05, 0xd1, 0x4a, 0xc0, 0x81, 0x81, 0xb1, 0x18, 0x00, 0x00, 0x20, 0xa0, 0x00, 0x50, 0x27, 0xfb,
    0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x13, 0x00, 0x00, 0x00, 0x10, 0x00, 0x02, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x01, 0xe4, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x61, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x00, 0x61, 0x54, 0x65, 0x78, 0x63, 0x6f,
    0x6f, 0x72, 0x64, 0x00, 0x77, 0x76, 0x70, 0x00,
};
static const unsigned char g_texture_f[228] __attribute__((aligned(16))) = {
    0x47, 0x58, 0x50, 0x00, 0x01, 0x04, 0x00, 0x00, 0xe4, 0x00, 0x00, 0x00, 0x9e, 0x63, 0xcb, 0xa0,
    0x7b, 0xc7, 0x33, 0x60, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa8, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xf9, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x44, 0xfa, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x02, 0x04, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x74, 0x65, 0x78, 0x00,
};

typedef struct {
    float x, y, z, u, v;
} vertex;

typedef struct {
    void *address;
} display_data;

static SceGxmContext *g_ctx;
static SceGxmRenderTarget *g_target;
static SceGxmShaderPatcher *g_patcher;
static SceGxmShaderPatcherId g_vid, g_fid;
static SceGxmVertexProgram *g_vprog;
static SceGxmFragmentProgram *g_fprog;
static const SceGxmProgramParameter *g_wvp;
static void *g_display[2];
static SceGxmColorSurface g_surface[2];
static SceGxmSyncObject *g_sync[2];
static SceGxmDepthStencilSurface g_depth;
static int g_back, g_front = 1;
static vertex *g_vertices[2];   /* the black quad and the picture quad, per texture */
static u16 *g_indices;
static SceGxmTexture g_tex[2], g_black;
static void *g_tex_mem[2];
static int g_tex_w, g_tex_h, g_tex_stride, g_tex_next;
static SceUID g_blocks[24];
static int g_block_count;
static void *g_context_host;
static int g_initialized;

/* GPU-mapped memory; CDRAM sizes go by 256 KiB, the others by 4 KiB. */
static void *gpu_alloc(SceKernelMemBlockType type, unsigned size, int attribs)
{
    void *mem;
    SceUID uid;
    size = ALIGN_UP(size, type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096);
    if (g_block_count >= (int)(sizeof(g_blocks) / sizeof(g_blocks[0])) ||
        (uid = sceKernelAllocMemBlock("xfgpu", type, size, NULL)) < 0)
        return NULL;
    g_blocks[g_block_count++] = uid;
    if (sceKernelGetMemBlockBase(uid, &mem) < 0 || sceGxmMapMemory(mem, size, attribs) < 0)
        return NULL;
    return mem;
}

static void *usse_alloc(unsigned size, unsigned *offset, int fragment)
{
    void *mem;
    SceUID uid;
    size = ALIGN_UP(size, 4096);
    if (g_block_count >= (int)(sizeof(g_blocks) / sizeof(g_blocks[0])) ||
        (uid = sceKernelAllocMemBlock("xfusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL)) < 0)
        return NULL;
    g_blocks[g_block_count++] = uid;
    if (sceKernelGetMemBlockBase(uid, &mem) < 0)
        return NULL;
    if ((fragment ? sceGxmMapFragmentUsseMemory(mem, size, offset) : sceGxmMapVertexUsseMemory(mem, size, offset)) < 0)
        return NULL;
    return mem;
}

static void *host_alloc(void *user, unsigned size)
{
    (void)user;
    return malloc(size);
}

static void host_free(void *user, void *mem)
{
    (void)user;
    free(mem);
}

static void display_callback(const void *data)
{
    const display_data *d = data;
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = d->address;
    fb.pitch = SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

int vita_gpu_init(void)
{
    SceGxmInitializeParams init;
    SceGxmContextParams cp;
    SceGxmRenderTargetParams rt;
    SceGxmShaderPatcherParams pp;
    SceGxmVertexAttribute attr[2];
    SceGxmVertexStream stream;
    void *vdm, *vertex_ring, *fragment_ring, *fragment_usse, *patcher_buf, *patcher_vusse, *patcher_fusse;
    unsigned fragment_usse_off, vusse_off, fusse_off;
    unsigned depth_w = ALIGN_UP(SCREEN_W, SCE_GXM_TILE_SIZEX), depth_h = ALIGN_UP(SCREEN_H, SCE_GXM_TILE_SIZEY);
    void *depth, *stencil, *black;
    int i;

    memset(&init, 0, sizeof(init));
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = sizeof(display_data);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (sceGxmInitialize(&init) < 0)
        return -1;
    g_initialized = 1;

    vdm = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    vertex_ring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,
                            SCE_GXM_MEMORY_ATTRIB_READ);
    fragment_ring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,
                              SCE_GXM_MEMORY_ATTRIB_READ);
    fragment_usse = usse_alloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &fragment_usse_off, 1);
    g_context_host = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    if (!vdm || !vertex_ring || !fragment_ring || !fragment_usse || !g_context_host)
        return -1;
    memset(&cp, 0, sizeof(cp));
    cp.hostMem = g_context_host;
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = vdm;
    cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = vertex_ring;
    cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = fragment_ring;
    cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = fragment_usse;
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = fragment_usse_off;
    if (sceGxmCreateContext(&cp, &g_ctx) < 0)
        return -1;

    memset(&rt, 0, sizeof(rt));
    rt.width = SCREEN_W;
    rt.height = SCREEN_H;
    rt.scenesPerFrame = 1;
    rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    rt.driverMemBlock = -1;
    if (sceGxmCreateRenderTarget(&rt, &g_target) < 0)
        return -1;

    for (i = 0; i < 2; i++) {
        g_display[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 4 * SCREEN_W * SCREEN_H,
                                 SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
        if (!g_display[i])
            return -1;
        memset(g_display[i], 0, 4 * SCREEN_W * SCREEN_H);
        if (sceGxmColorSurfaceInit(&g_surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR,
                                   SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCREEN_W,
                                   SCREEN_H, SCREEN_W, g_display[i]) < 0 ||
            sceGxmSyncObjectCreate(&g_sync[i]) < 0)
            return -1;
    }
    depth = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * depth_w * depth_h,
                      SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    stencil = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * depth_w * depth_h,
                        SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    if (!depth || !stencil ||
        sceGxmDepthStencilSurfaceInit(&g_depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                      depth_w, depth, stencil) < 0)
        return -1;

    patcher_buf = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * 1024,
                            SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    patcher_vusse = usse_alloc(64 * 1024, &vusse_off, 0);
    patcher_fusse = usse_alloc(64 * 1024, &fusse_off, 1);
    if (!patcher_buf || !patcher_vusse || !patcher_fusse)
        return -1;
    memset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = host_alloc;
    pp.hostFreeCallback = host_free;
    pp.bufferMem = patcher_buf;
    pp.bufferMemSize = 64 * 1024;
    pp.vertexUsseMem = patcher_vusse;
    pp.vertexUsseMemSize = 64 * 1024;
    pp.vertexUsseOffset = vusse_off;
    pp.fragmentUsseMem = patcher_fusse;
    pp.fragmentUsseMemSize = 64 * 1024;
    pp.fragmentUsseOffset = fusse_off;
    if (sceGxmShaderPatcherCreate(&pp, &g_patcher) < 0 ||
        sceGxmProgramCheck((const SceGxmProgram *)g_texture_v) < 0 ||
        sceGxmProgramCheck((const SceGxmProgram *)g_texture_f) < 0 ||
        sceGxmShaderPatcherRegisterProgram(g_patcher, (const SceGxmProgram *)g_texture_v, &g_vid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(g_patcher, (const SceGxmProgram *)g_texture_f, &g_fid) < 0)
        return -1;

    /* position (3 floats) then texture coordinates (2 floats) */
    attr[0].streamIndex = 0;
    attr[0].offset = 0;
    attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 3;
    attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(
        sceGxmProgramFindParameterByName((const SceGxmProgram *)g_texture_v, "aPosition"));
    attr[1].streamIndex = 0;
    attr[1].offset = 12;
    attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[1].componentCount = 2;
    attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(
        sceGxmProgramFindParameterByName((const SceGxmProgram *)g_texture_v, "aTexcoord"));
    stream.stride = sizeof(vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(g_patcher, g_vid, attr, 2, &stream, 1, &g_vprog) < 0 ||
        sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                 SCE_GXM_MULTISAMPLE_NONE, NULL, (const SceGxmProgram *)g_texture_v,
                                                 &g_fprog) < 0)
        return -1;
    g_wvp = sceGxmProgramFindParameterByName((const SceGxmProgram *)g_texture_v, "wvp");

    for (i = 0; i < 2; i++)
        if (!(g_vertices[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 8 * sizeof(vertex),
                                        SCE_GXM_MEMORY_ATTRIB_READ)))
            return -1;
    if (!(g_indices = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * sizeof(u16), SCE_GXM_MEMORY_ATTRIB_READ)))
        return -1;
    for (i = 0; i < 4; i++)
        g_indices[i] = (u16)i;

    /* an 8x8 black texture for the background */
    if (!(black = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 8 * 8 * 4, SCE_GXM_MEMORY_ATTRIB_READ)))
        return -1;
    memset(black, 0, 8 * 8 * 4);
    if (sceGxmTextureInitLinear(&g_black, black, SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB, 8, 8, 0) < 0)
        return -1;
    sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    return 0;
}

void vita_gpu_shutdown(void)
{
    int i;
    if (!g_initialized)
        return;
    if (g_ctx) {
        sceGxmFinish(g_ctx);
        sceGxmDisplayQueueFinish();
    }
    if (g_patcher) {
        if (g_vprog)
            sceGxmShaderPatcherReleaseVertexProgram(g_patcher, g_vprog);
        if (g_fprog)
            sceGxmShaderPatcherReleaseFragmentProgram(g_patcher, g_fprog);
        sceGxmShaderPatcherUnregisterProgram(g_patcher, g_vid);
        sceGxmShaderPatcherUnregisterProgram(g_patcher, g_fid);
        sceGxmShaderPatcherDestroy(g_patcher);
    }
    for (i = 0; i < 2; i++)
        if (g_sync[i])
            sceGxmSyncObjectDestroy(g_sync[i]);
    if (g_target)
        sceGxmDestroyRenderTarget(g_target);
    if (g_ctx)
        sceGxmDestroyContext(g_ctx);
    for (i = 0; i < g_block_count; i++) {
        void *mem;
        if (sceKernelGetMemBlockBase(g_blocks[i], &mem) >= 0)
            sceGxmUnmapMemory(mem);
        sceKernelFreeMemBlock(g_blocks[i]);
    }
    g_block_count = 0;
    free(g_context_host);
    g_context_host = NULL;
    g_ctx = NULL;
    g_patcher = NULL;
    g_target = NULL;
    g_vprog = NULL;
    g_fprog = NULL;
    g_sync[0] = g_sync[1] = NULL;
    g_tex_mem[0] = g_tex_mem[1] = NULL;
    g_tex_w = g_tex_h = 0;
    g_initialized = 0;
    sceGxmTerminate();
}

/* Two textures of the frame's size, written in turns: the GPU may still be
 * reading the other. Their memory takes the largest frame, the screen's;
 * rows go 8-pixel aligned. */
static int make_textures(int width, int height)
{
    int i;
    if (width == g_tex_w && height == g_tex_h)
        return 0;
    if (width > SCREEN_W || height > SCREEN_H)
        return -1;
    if (g_tex_w)
        sceGxmFinish(g_ctx); /* the old textures may be in use */
    for (i = 0; i < 2; i++) {
        if (!g_tex_mem[i] &&
            !(g_tex_mem[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SCREEN_W * SCREEN_H * 4,
                                       SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)))
            return -1;
        if (sceGxmTextureInitLinear(&g_tex[i], g_tex_mem[i], SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB, (unsigned)width,
                                    (unsigned)height, 0) < 0)
            return -1;
        sceGxmTextureSetMinFilter(&g_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&g_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    g_tex_stride = ALIGN_UP(width, 8);
    g_tex_w = width;
    g_tex_h = height;
    return 0;
}

/* A quad from screen pixels x0, y0 to x1, y1, in clip space. */
static void quad(vertex *v, float x0, float y0, float x1, float y1)
{
    float cx0 = x0 / (SCREEN_W / 2.0f) - 1.0f, cx1 = x1 / (SCREEN_W / 2.0f) - 1.0f;
    float cy0 = 1.0f - y0 / (SCREEN_H / 2.0f), cy1 = 1.0f - y1 / (SCREEN_H / 2.0f);
    v[0].x = cx0, v[0].y = cy0, v[0].u = 0.0f, v[0].v = 0.0f;
    v[1].x = cx1, v[1].y = cy0, v[1].u = 1.0f, v[1].v = 0.0f;
    v[2].x = cx0, v[2].y = cy1, v[2].u = 0.0f, v[2].v = 1.0f;
    v[3].x = cx1, v[3].y = cy1, v[3].u = 1.0f, v[3].v = 1.0f;
    v[0].z = v[1].z = v[2].z = v[3].z = 0.5f;
}

void vita_gpu_present(const u32 *pixels, int width, int height, int pitch, int x, int y, int w, int h)
{
    static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    display_data dd;
    void *uniforms;
    u8 *dst;
    vertex *v;
    int row, t;
    if (!g_ctx || make_textures(width, height) != 0)
        return;
    t = g_tex_next;
    g_tex_next ^= 1;
    /* the frame's rows, copied whole: the texture is in uncached CDRAM */
    dst = g_tex_mem[t];
    for (row = 0; row < height; row++)
        memcpy(dst + (size_t)row * g_tex_stride * 4, pixels + (size_t)row * (size_t)pitch, (size_t)width * 4);
    v = g_vertices[t];
    quad(v, 0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H);
    quad(v + 4, (float)x, (float)y, (float)(x + w), (float)(y + h));

    sceGxmBeginScene(g_ctx, 0, g_target, NULL, NULL, g_sync[g_back], &g_surface[g_back], &g_depth);
    sceGxmSetVertexProgram(g_ctx, g_vprog);
    sceGxmSetFragmentProgram(g_ctx, g_fprog);
    sceGxmReserveVertexDefaultUniformBuffer(g_ctx, &uniforms);
    sceGxmSetUniformDataF(uniforms, g_wvp, 0, 16, identity);
    sceGxmSetFragmentTexture(g_ctx, 0, &g_black);
    sceGxmSetVertexStream(g_ctx, 0, v);
    sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, g_indices, 4);
    sceGxmReserveVertexDefaultUniformBuffer(g_ctx, &uniforms);
    sceGxmSetUniformDataF(uniforms, g_wvp, 0, 16, identity);
    sceGxmSetFragmentTexture(g_ctx, 0, &g_tex[t]);
    sceGxmSetVertexStream(g_ctx, 0, v + 4);
    sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, g_indices, 4);
    sceGxmEndScene(g_ctx, NULL, NULL);
    sceGxmPadHeartbeat(&g_surface[g_back], g_sync[g_back]);

    dd.address = g_display[g_back];
    sceGxmDisplayQueueAddEntry(g_sync[g_front], g_sync[g_back], &dd);
    g_front = g_back;
    g_back ^= 1;
}
