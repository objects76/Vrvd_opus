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
 *   ScapQuantInit(&q);                    - once, before first use
 *   idx = ScapQuantPixel(&q, px, x, y);   - BGRA32 pixel at frame coords
 *   ScapQuantRect(&q, src, stride, dst, w, h, x0, y0);
 *                                         - whole rect at frame coords (x0,y0);
 *                                           same output as per-pixel calls but
 *                                           takes the fast path (AVX2 on the
 *                                           332 backend when the CPU has it)
 *
 * The switch lives HERE (not per-project) so scapenc and scapdec can never
 * disagree on the palette: encoder and decoder must be built with the same
 * backend or every decoded color is wrong.
 */
#pragma once

#define SCAP_USE_256MAP

#ifdef SCAP_USE_256MAP

#include "scap_256map.h"

typedef struct { uint8_t lut[32768]; } ScapQuant;

static __inline void ScapQuantInit(ScapQuant* q)
{
    ScapBuildLut(q->lut);
}

static __inline uint8_t ScapQuantPixel(const ScapQuant* q, const uint8_t* p,
                                       int x, int y)
{
    (void)x; (void)y; /* nearest-match mapping is position-independent */
    return q->lut[ScapC32_15(p)];
}

static __inline void ScapQuantRect(const ScapQuant* q, const uint8_t* src,
                                   int srcStride, uint8_t* dst, int w, int h,
                                   int x0, int y0)
{
    (void)x0; (void)y0;
    for (int row = 0; row < h; ++row, src += srcStride)
    {
        const uint8_t* px = src;
        for (int x = 0; x < w; ++x, px += 4)
            *dst++ = q->lut[ScapC32_15(px)];
    }
}

#else /* default: RGB332 + Bayer dithering */

#include "scap_332dither.h"

typedef struct { int avx2; } ScapQuant; /* dispatch decided once at init */

static __inline void ScapQuantInit(ScapQuant* q)
{
    q->avx2 = ScapQuant332DetectAvx2();
}

static __inline uint8_t ScapQuantPixel(const ScapQuant* q, const uint8_t* p,
                                       int x, int y)
{
    (void)q;
    return ScapQuant332(p, x, y);
}

static __inline void ScapQuantRect(const ScapQuant* q, const uint8_t* src,
                                   int srcStride, uint8_t* dst, int w, int h,
                                   int x0, int y0)
{
    ScapQuant332Rect(src, srcStride, dst, w, h, x0, y0, q->avx2);
}

#endif
