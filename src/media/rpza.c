#include "rpza.h"

#include "xf_endian.h"

static u32 rgb555(u16 c)
{
    u32 r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

typedef struct {
    u32 *frame;
    int width, height, bw, block, total;
} rpza_out;

static u32 *block_pixel(rpza_out *o, int px, int py)
{
    int x = (o->block % o->bw) * 4 + px, y = (o->block / o->bw) * 4 + py;
    if (x >= o->width || y >= o->height)
        return NULL;
    return &o->frame[y * o->width + x];
}

static void fill_block(rpza_out *o, u32 color)
{
    int x, y;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            u32 *p = block_pixel(o, x, y);
            if (p)
                *p = color;
        }
}

int xf_rpza_decode(const u8 *data, size_t size, u32 *frame, int width, int height)
{
    const u8 *p = data + 4, *end = data + size;
    rpza_out o;

    if (size < 4 || data[0] != 0xE1)
        return -1;
    if ((xf_be32(data) & 0x00FFFFFF) < size)
        end = data + (xf_be32(data) & 0x00FFFFFF);
    o.frame = frame;
    o.width = width;
    o.height = height;
    o.bw = (width + 3) / 4;
    o.total = o.bw * ((height + 3) / 4);
    o.block = 0;

    while (p < end && o.block < o.total) {
        int op = *p++, n = (op & 0x1F) + 1, i, x, y;
        u16 a = 0, b;

        if (!(op & 0x80)) {
            /* No opcode: the byte starts color A. A set top bit on the next
             * color means a 4-color block, otherwise a 16-color block. */
            if (p + 1 > end)
                return -1;
            a = (u16)(op << 8 | *p++);
            op = 0;
            n = 1;
            if (p < end && (*p & 0x80))
                op = 0x20;
        }
        switch (op & 0xE0) {
        case 0x80: /* skip blocks */
            o.block += n;
            break;
        case 0xA0: /* one color */
            if (p + 2 > end)
                return -1;
            a = xf_be16(p);
            p += 2;
            for (i = 0; i < n && o.block < o.total; i++, o.block++)
                fill_block(&o, rgb555(a));
            break;
        case 0xC0: /* four colors, with color A given */
        case 0x20: { /* four colors */
            u32 c4[4];
            int ta, tb, k;
            if (op & 0x40) {
                if (p + 2 > end)
                    return -1;
                a = xf_be16(p);
                p += 2;
            }
            if (p + 2 > end)
                return -1;
            b = xf_be16(p);
            p += 2;
            {
                u16 m[4] = { b, 0, 0, a };
                for (k = 0; k < 3; k++) {
                    int shift = 10 - k * 5;
                    ta = a >> shift & 31;
                    tb = b >> shift & 31;
                    m[1] |= (u16)(((11 * ta + 21 * tb) >> 5) << shift);
                    m[2] |= (u16)(((21 * ta + 11 * tb) >> 5) << shift);
                }
                for (k = 0; k < 4; k++)
                    c4[k] = rgb555(m[k]);
            }
            for (i = 0; i < n && o.block < o.total; i++, o.block++) {
                if (p + 4 > end)
                    return -1;
                for (y = 0; y < 4; y++, p++)
                    for (x = 0; x < 4; x++) {
                        u32 *px = block_pixel(&o, x, y);
                        if (px)
                            *px = c4[*p >> (6 - x * 2) & 3];
                    }
            }
            break;
        }
        case 0x00: /* sixteen colors, the first one is color A */
            if (p + 30 > end)
                return -1;
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    u32 *px = block_pixel(&o, x, y);
                    u16 c = (x || y) ? xf_be16(p) : a;
                    if (x || y)
                        p += 2;
                    if (px)
                        *px = rgb555(c);
                }
            o.block++;
            break;
        default:
            return -1;
        }
    }
    return 0;
}
