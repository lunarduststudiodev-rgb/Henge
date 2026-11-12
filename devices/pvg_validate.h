#ifndef HENGEVM_PVG_VALIDATE_H
#define HENGEVM_PVG_VALIDATE_H

/* Texture descriptions from the guest, checked before Metal sees them. Metal's own checks of a
   description are assertions: a bad one stops the whole app, whatever the guest meant by it. So each
   rule here is one of those, measured against Metal (macOS 27, Apple GPU) with descriptions made at
   random until none got through: what passes here Metal takes, or refuses with nil. Plain C, so the
   rules can be tested without a GPU. Numbers are Metal's (MTLPixelFormat, MTLTextureType...). */

#include <stdint.h>

typedef struct HvTexDesc {
    uint32_t type, format, width, height, depth, mips, samples, array;
    uint64_t usage, options; /* options: MTLResourceOptions, as the description's resourceOptions */
} HvTexDesc;

/* What this GPU can do, from its MTLDevice: samples has bit n set when it takes n samples. color_bytes: the most a
   sample of a pass's colour attachments may take (the implicit imageblock); implicit_bytes: the implicit imageblock
   memory of a tile. 0: not checked (a GPU without tile memory). float32_filter: it filters 32-bit float formats. */
typedef struct HvTexCaps {
    int bc;
    uint32_t samples, color_bytes, implicit_bytes;
    int float32_filter;
} HvTexCaps;

enum { HV_TEX_1D = 0, HV_TEX_1D_ARRAY, HV_TEX_2D, HV_TEX_2D_ARRAY, HV_TEX_2D_MS, HV_TEX_CUBE, HV_TEX_CUBE_ARRAY, HV_TEX_3D,
       HV_TEX_2D_MS_ARRAY, HV_TEX_BUFFER };

/* Formats Metal knows here (any other is an assertion). 255 and 262 are Mac-only, mapped to 260/261 on iOS. */
static inline int hv_pvg_format_known(uint32_t f) {
    switch (f) {
    case 1: case 10 ... 14: case 20: case 22 ... 26: case 30 ... 34: case 40 ... 44: case 53 ... 55: case 60: case 62 ... 66:
    case 70 ... 74: case 80: case 81: case 84: case 85: case 90 ... 94: case 103 ... 105: case 110: case 112 ... 116: case 123 ... 125:
    case 130 ... 135: case 140 ... 143: case 150 ... 153: case 160 ... 167: case 170: case 172: case 174: case 176: case 178 ... 183:
    case 185 ... 200: case 203 ... 218: case 221 ... 236: case 240: case 241: case 250: case 252: case 253: case 255: case 260 ... 263:
    case 300 ... 304: case 313 ... 315: case 323 ... 325: case 500 ... 510: case 520 ... 530: case 540 ... 548: case 550 ... 556:
    case 560: case 562 ... 613: case 615 ... 652:
        return 1;
    default: return 0;
    }
}

/* Bytes a pixel of a format that can sit on a buffer (a linear texture); 0 for the rest. */
static inline uint32_t hv_pvg_linear_bpp(uint32_t f) {
    switch (f) {
    case 1: case 10 ... 14: case 501: case 521: case 562: case 588: case 643: case 647: return 1;
    case 20: case 22 ... 26: case 30 ... 34: case 40 ... 44: case 504: case 524: case 550: case 551: case 563: case 576: case 577:
    case 589: case 590: case 603: case 604: case 606: case 607: case 621: case 622: case 624 ... 627: case 642: case 645: case 646:
    case 649: case 650: return 2;
    case 53 ... 55: case 60: case 62 ... 66: case 70 ... 74: case 80: case 81: case 84: case 85: case 90 ... 94: case 300 ... 304:
    case 554: case 555: case 560: case 578: case 579: case 586: case 587: case 605: case 623: case 628: case 629: case 644: case 648:
        return 4;
    case 103 ... 105: case 110: case 112 ... 116: case 313 ... 315: case 552: case 553: case 630 ... 641: return 8;
    case 123 ... 125: case 323 ... 325: return 16;
    default: return 0;
    }
}

/* What a format can't be, by class */
enum { HV_PF_NO_MSAA = 1, HV_PF_NO_1D = 2, HV_PF_NO_3D = 4, HV_PF_PVRTC = 8, HV_PF_422 = 16 };
static inline int hv_pvg_format_limits(uint32_t f) {
    switch (f) {
    case 1 ... 25: case 30 ... 65: case 70 ... 115: case 117 ... 125: case 300 ... 325: case 552 ... 555: case 586: case 587: case 623:
    case 628: case 629: case 631 ... 636: case 638 ... 641: return 0;
    case 130 ... 153: case 185 ... 236: return HV_PF_NO_MSAA | HV_PF_NO_1D; /* BC, ASTC */
    case 170 ... 183: case 651: case 652: return HV_PF_NO_MSAA | HV_PF_NO_1D | HV_PF_NO_3D; /* ETC2, EAC */
    case 160 ... 167: return HV_PF_NO_MSAA | HV_PF_NO_1D | HV_PF_NO_3D | HV_PF_PVRTC;
    case 250 ... 263: return HV_PF_NO_1D | HV_PF_NO_3D; /* depth, stencil */
    case 240: case 241: return HV_PF_NO_MSAA | HV_PF_NO_1D | HV_PF_NO_3D | HV_PF_422;
    default: return HV_PF_NO_MSAA; /* video and the other formats Apple keeps to itself */
    }
}

static inline int hv_pvg_type_is_array(uint32_t t) {
    return t == HV_TEX_1D_ARRAY || t == HV_TEX_2D_ARRAY || t == HV_TEX_CUBE_ARRAY || t == HV_TEX_2D_MS_ARRAY;
}

/* Usage bits Metal takes: its public ones (0x3f) and 0x10000, a private one macOS 26/27's WindowServer sets on the
   textures it blurs Liquid Glass into (Metal on macOS 27 and iPadOS takes it). Most other bits past 0x3f abort
   (measured on macOS 27: 0x40...0x2000 and 0x20000 up do; 0x4000 and 0x8000 don't, but no guest is seen to send them).
   pvg_metal.m drops any other bit before the checks, so the texture is still made: a usage bit is a hint. */
#define HV_PVG_USAGE_OK 0x1003full

/* NULL if Metal takes the description, else what is wrong with it. */
static inline const char *hv_pvg_texture_problem(const HvTexDesc *d, HvTexCaps caps) {
    uint32_t f = d->format, type = d->type, w = d->width, h = d->height, depth = d->depth, mips = d->mips;
    if (!hv_pvg_format_known(f) || (f >= 130 && f <= 153 && !caps.bc)) return "pixel format";
    if (type > HV_TEX_BUFFER) return "type";
    if (!w || !h || !depth || !d->array || !d->samples || !mips) return "a size of zero";
    if (d->usage & ~(uint64_t)HV_PVG_USAGE_OK) return "usage";
    if ((d->usage & 0x20) && f != 53 && f != 54 && f != 103) return "atomic usage for the format";
    if ((d->options & 0xf) > 1 || ((d->options >> 4) & 0xf) > 3 || ((d->options >> 8) & 0x3) > 2 || (d->options & ~(uint64_t)0x3ff))
        return "resource options";
    int limits = hv_pvg_format_limits(f);
    int one = type == HV_TEX_1D || type == HV_TEX_1D_ARRAY, three = type == HV_TEX_3D;
    int ms = type == HV_TEX_2D_MS || type == HV_TEX_2D_MS_ARRAY;
    uint32_t max = three ? 2048 : 16384;
    if (w > max || h > max || depth > max) return "too big";
    if ((one || type == HV_TEX_BUFFER) && h != 1) return "height";
    if (!three && depth != 1) return "depth";
    if ((type == HV_TEX_CUBE || type == HV_TEX_CUBE_ARRAY) && w != h) return "cube not square";
    if (hv_pvg_type_is_array(type) ? d->array > 2048 : d->array != 1) return "array length";
    if (ms ? d->samples < 2 || d->samples > 31 || mips != 1 || (limits & HV_PF_NO_MSAA) || !(caps.samples >> d->samples & 1)
           : d->samples != 1)
        return "sample count";
    int buffer = type == HV_TEX_BUFFER; /* a texture buffer: rows of plain pixels, as 1D */
    if (((one || buffer) && (limits & HV_PF_NO_1D)) || (three && (limits & HV_PF_NO_3D)) || (buffer && !hv_pvg_linear_bpp(f)))
        return "type for the format";
    if ((limits & HV_PF_PVRTC) && (w != h || (w & (w - 1)))) return "PVRTC size";
    if ((limits & HV_PF_422) && (type != HV_TEX_2D || (w & 1) || mips != 1)) return "4:2:2 layout";
    uint32_t big = w > h ? w : h, levels = 1;
    if (three && depth > big) big = depth;
    while (big >>= 1) levels++;
    if (mips > (one || buffer ? 1 : levels)) return "mip count";
    return 0;
}

static inline int hv_pvg_view_type_ok(uint32_t from, uint32_t to) {
    switch (from) {
    case HV_TEX_1D: case HV_TEX_1D_ARRAY: return to == HV_TEX_1D || to == HV_TEX_1D_ARRAY;
    case HV_TEX_2D: return to == HV_TEX_2D || to == HV_TEX_2D_ARRAY;
    case HV_TEX_2D_ARRAY: case HV_TEX_CUBE: case HV_TEX_CUBE_ARRAY:
        return to == HV_TEX_2D || to == HV_TEX_2D_ARRAY || to == HV_TEX_CUBE || to == HV_TEX_CUBE_ARRAY;
    case HV_TEX_2D_MS: case HV_TEX_2D_MS_ARRAY: return to == HV_TEX_2D_MS || to == HV_TEX_2D_MS_ARRAY;
    default: return to == from; /* 3D, a texture buffer */
    }
}

/* Metal's own colour formats cast to another of their size (its private ones only to themselves),
   XR to its sRGB form, compressed ones to theirs, and depth-and-stencil to its stencil. */
static inline int hv_pvg_view_format_ok(uint32_t from, uint32_t f) {
    if (f == from) return 1;
    int pa = from < 130 && from != 26 && from != 44 && from != 66 && from != 84 && from != 85 && from != 116;
    int pb = f < 130 && f != 26 && f != 44 && f != 66 && f != 84 && f != 85 && f != 116;
    if (pa && pb && hv_pvg_linear_bpp(from) == hv_pvg_linear_bpp(f)) return 1;
    if (from >= 552 && from <= 555 && (from ^ 1) == f) return 1;
    uint32_t lo = from < f ? from : f, hi = from < f ? f : from;
    if (lo >= 130 && hi <= 183 && !(lo & 1) && hi == lo + 1 && lo != 140 && lo != 142 && lo != 150 && lo != 170 && lo != 172 &&
        lo != 174 && lo != 176)
        return 1;
    if (lo >= 186 && lo <= 200 && hi == lo + 18) return 1;
    if ((from == 260 || from == 255 || from == 261 || from == 262) && (f == 260 || f == 255 || f == 261 || f == 262)) return 1;
    return 0;
}

/* A view of parent (as made) with this format, type, levels and slices. */
static inline const char *hv_pvg_view_problem(const HvTexDesc *parent, uint32_t f, uint32_t type, uint64_t level, uint64_t levels,
                                              uint64_t slice, uint64_t slices) {
    if (!hv_pvg_format_known(f) || !hv_pvg_view_format_ok(parent->format, f)) return "pixel format";
    if (!hv_pvg_view_type_ok(parent->type, type)) return "type";
    uint64_t have = (uint64_t)parent->array * (parent->type == HV_TEX_CUBE || parent->type == HV_TEX_CUBE_ARRAY ? 6 : 1);
    if (!levels || level > parent->mips || levels > parent->mips - level) return "levels";
    if (!slices || slice > have || slices > have - slice) return "slices";
    int array = type == HV_TEX_1D_ARRAY || type == HV_TEX_2D_ARRAY || type == HV_TEX_2D_MS_ARRAY;
    if (type == HV_TEX_CUBE ? slices != 6 : type == HV_TEX_CUBE_ARRAY ? slices % 6 != 0 : !array && slices != 1)
        return "slices for the type";
    return 0;
}

/* A texture over a buffer of buffer_len bytes, its first row at offset, row bytes apart. align is
   the device's minimumLinearTextureAlignmentForPixelFormat. */
static inline const char *hv_pvg_linear_problem(const HvTexDesc *d, HvTexCaps caps, uint64_t buffer_len, uint64_t offset, uint64_t row,
                                                uint64_t align) {
    const char *why = hv_pvg_texture_problem(d, caps);
    if (why) return why;
    uint32_t bpp = hv_pvg_linear_bpp(d->format);
    if (!bpp || (d->type != HV_TEX_2D && d->type != HV_TEX_BUFFER) || d->mips != 1) return "linear layout";
    uint64_t last_row = (uint64_t)d->width * bpp;
    if (row < last_row || (align && (row % align || offset % align))) return "bytes per row";
    if (offset > buffer_len) return "past the end of the buffer";
    if (d->height > 1 && row > (UINT64_MAX - last_row) / (d->height - 1)) return "past the end of the buffer";
    uint64_t needed = d->height ? (uint64_t)(d->height - 1) * row + last_row : 0;
    if (needed > buffer_len - offset) return "past the end of the buffer";
    return 0;
}

#endif
