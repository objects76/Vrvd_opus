/* scap_palette.h - fixed RGB332 palette + BGRA32->8bpp quantizer with 4x4
 * Bayer ordered dithering (replaces the legacy 240-color nearest-match LUT;
 * rationale in the 2026-07-08 research note under research/).
 *
 * Index layout: RRRGGGBB. Both scapenc and scapdec share this header; the
 * palette is never transmitted. The dither pattern depends only on absolute
 * frame coordinates, so the same input always yields the same output
 * (dirty-rect / tile-hash safe).
 */
#pragma once
#include <stdint.h>

typedef struct { uint8_t r, g, b; } ScapPalEntry;

/* channel expansion: 3-bit -> 0,36,73,109,146,182,219,255; 2-bit -> 0,85,170,255 */
#define SCAP_E3(v) (uint8_t)(((v) << 5) | ((v) << 2) | ((v) >> 1))
#define SCAP_E2(v) (uint8_t)((v) * 0x55)
#define SCAP_B4(r, g) \
    { SCAP_E3(r), SCAP_E3(g), SCAP_E2(0) }, { SCAP_E3(r), SCAP_E3(g), SCAP_E2(1) }, \
    { SCAP_E3(r), SCAP_E3(g), SCAP_E2(2) }, { SCAP_E3(r), SCAP_E3(g), SCAP_E2(3) }
#define SCAP_G32(r) \
    SCAP_B4(r, 0), SCAP_B4(r, 1), SCAP_B4(r, 2), SCAP_B4(r, 3), \
    SCAP_B4(r, 4), SCAP_B4(r, 5), SCAP_B4(r, 6), SCAP_B4(r, 7)

static const ScapPalEntry kScapPal[256] = {
    SCAP_G32(0), SCAP_G32(1), SCAP_G32(2), SCAP_G32(3),
    SCAP_G32(4), SCAP_G32(5), SCAP_G32(6), SCAP_G32(7)
};

/* 4x4 Bayer threshold map, values 0..15 */
static const uint8_t kScapBayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* BGRA32 pixel (bytes B,G,R,A) at absolute frame coords (x,y) -> RGB332
 * index. Ordered dither: add the Bayer offset scaled to the channel's
 * quantization step (32 for R/G, 64 for B), clamp, then truncate to the
 * channel's top bits. */
static __inline uint8_t ScapQuant332(const uint8_t* p, int x, int y)
{
    int m = kScapBayer[y & 3][x & 3];
    int r = p[2] + m * 2, g = p[1] + m * 2, b = p[0] + m * 4;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint8_t)((r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6));
}
