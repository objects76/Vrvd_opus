/* scap_palette.h - backend selector + adapter for the 32bpp->8bpp quantizer.
 * Consumers include only this header; it pulls in exactly one backend:
 *
 *   scap_332dither.h (default) - RGB332 palette + 4x4 Bayer ordered dithering
 *   scap_256map.h              - legacy 240-color palette + nearest-match LUT
 *                                (define SCAP_USE_256MAP below to select)
 *
 * Both backends provide kScapPal[256]/ScapPalEntry for the decode side.
 * The encode side goes through the adapter below, hiding the backend
 * difference (256map needs a 32KB LUT built once; 332 is stateless but
 * needs absolute frame coordinates for the dither pattern):
 *
 *   ScapQuant q;                          - quantizer state
 *   ScapQuantInit(&q, dither);            - once, before first use
 *   idx = ScapQuantPixel(&q, px, x, y);   - BGRA32 pixel at frame coords
 *   ScapQuantRect(&q, src, stride, dst, w, h, x0, y0);
 *                                         - whole rect at frame coords (x0,y0);
 *                                           same output as per-pixel calls but
 *                                           takes the fast path (AVX2 on the
 *                                           332 backend when the CPU has it)
 *
 * dither is a RUNTIME flag (codec spec "zstd[:level][:dither]"): ordered 4x4
 * Bayer dithering, keyed to absolute frame coords so the same input always
 * yields the same index (dirty-rect safe). Encoder-side only - it changes
 * which indices get picked, never the palette - so the decoder needs no
 * matching flag.
 *
 * The backend switch lives HERE (not per-project) so scapenc and scapdec can
 * never disagree on the palette: encoder and decoder must be built with the
 * same backend or every decoded color is wrong.
 */
#pragma once

#define SCAP_USE_256MAP

#ifdef SCAP_USE_256MAP

#include "scap_256map.h"

typedef struct { uint8_t lut[32768]; int dither; } ScapQuant;

/* 4x4 Bayer threshold map (same values as scap_332dither.h; redeclared here
 * because a TU includes exactly one backend header). */
static const uint8_t kScapQuantBayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static __inline void ScapQuantInit(ScapQuant* q, int dither)
{
    ScapBuildLut(q->lut);
    q->dither = dither;
}

static __inline uint8_t ScapQuantPixel(const ScapQuant* q, const uint8_t* p,
                                       int x, int y)
{
    if (!q->dither) /* nearest-match mapping is position-independent */
        return q->lut[ScapC32_15(p)];
    /* Centered ordered dither before the nearest-match LUT: offset all
     * channels by (m-8)*3 = -24..+21, ~half the palette's dominant 0x33
     * spacing, then clamp and pack to RGB555.
     * ponytail: one amplitude for the whole non-uniform palette - dense
     * regions (the near-grays) get slightly over-dithered; a per-region
     * amplitude LUT would be the upgrade path. */
    int o = ((int)kScapQuantBayer[y & 3][x & 3] - 8) * 3;
    int b = p[0] + o, g = p[1] + o, r = p[2] + o;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    return q->lut[((unsigned)(r & 0xF8) << 7) | ((unsigned)(g & 0xF8) << 2) |
                  ((unsigned)b >> 3)];
}

static __inline void ScapQuantRect(const ScapQuant* q, const uint8_t* src,
                                   int srcStride, uint8_t* dst, int w, int h,
                                   int x0, int y0)
{
    for (int row = 0; row < h; ++row, src += srcStride)
    {
        const uint8_t* px = src;
        for (int x = 0; x < w; ++x, px += 4)
            *dst++ = ScapQuantPixel(q, px, x0 + x, y0 + row);
    }
}

#else /* default: RGB332 + Bayer dithering */

#include "scap_332dither.h"

typedef struct { int avx2; int dither; } ScapQuant; /* decided once at init */

static __inline void ScapQuantInit(ScapQuant* q, int dither)
{
    q->avx2 = ScapQuant332DetectAvx2();
    q->dither = dither;
}

static __inline uint8_t ScapQuantPixel(const ScapQuant* q, const uint8_t* p,
                                       int x, int y)
{
    /* no dither: fixed half-step threshold = plain round-to-nearest */
    return q->dither ? ScapQuant332(p, x, y) : ScapQuant332M(p, 8);
}

static __inline void ScapQuantRect(const ScapQuant* q, const uint8_t* src,
                                   int srcStride, uint8_t* dst, int w, int h,
                                   int x0, int y0)
{
    if (q->dither)
    {
        ScapQuant332Rect(src, srcStride, dst, w, h, x0, y0, q->avx2);
        return;
    }
    /* ponytail: no-dither mode has no AVX2 path (scalar only); add a flat-
     * threshold vector variant of ScapQuant332Rect if it ever matters. */
    for (int row = 0; row < h; ++row, src += srcStride)
    {
        const uint8_t* px = src;
        for (int x = 0; x < w; ++x, px += 4)
            *dst++ = ScapQuant332M(px, 8);
    }
}

#endif
