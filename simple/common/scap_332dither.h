/* scap_332dither.h - fixed RGB332 palette + BGRA32->8bpp quantizer with 4x4
 * Bayer ordered dithering (replaces the legacy 240-color nearest-match LUT,
 * preserved in scap_256map.h; rationale in the 2026-07-08 research note
 * under research/). Defines the same ScapPalEntry/kScapPal symbols as
 * scap_256map.h, so a translation unit includes exactly one of the two.
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

/* ---- rect-level conversion --------------------------------------------
 * Convert a w*h BGRA rect (srcStride bytes per row) whose top-left sits at
 * absolute frame coords (x0,y0) into w*h tightly-packed indices. Output is
 * byte-exact identical to calling ScapQuant332 per pixel; the AVX2 path is
 * just faster (~6x measured on a 4K frame, memory-bandwidth bound). */

static __inline void ScapQuant332RectScalar(const uint8_t* src, int srcStride,
                                            uint8_t* dst, int w, int h,
                                            int x0, int y0)
{
    for (int row = 0; row < h; ++row, src += srcStride)
    {
        const uint8_t* px = src;
        for (int x = 0; x < w; ++x, px += 4)
            *dst++ = ScapQuant332(px, x0 + x, y0 + row);
    }
}

/* MSVC compiles AVX2 intrinsics without /arch:AVX2, so one TU can hold both
 * paths behind a runtime check. The encoder is Windows-only (DXGI); other
 * compilers (Linux viewer) never quantize and just get the scalar path. */
#if defined(_MSC_VER) && defined(_M_X64)
#define SCAP_332_AVX2 1
#include <immintrin.h>
#include <intrin.h>

/* AVX2 supported by the CPU and YMM state enabled by the OS? */
static __inline int ScapQuant332DetectAvx2(void)
{
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7)
        return 0;
    __cpuid(r, 1); /* ECX: bit27 OSXSAVE, bit28 AVX */
    if ((r[2] & (1 << 27)) == 0 || (r[2] & (1 << 28)) == 0)
        return 0;
    if ((_xgetbv(0) & 6) != 6) /* XCR0: XMM+YMM state */
        return 0;
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1; /* EBX bit5: AVX2 */
}

/* 8 px/iter: one saturating byte add applies dither offset AND clamp for all
 * channels; the Bayer period (4) divides the vector width (8), so the dither
 * vector is a row constant regardless of x0 phase. */
static __inline void ScapQuant332RectAvx2(const uint8_t* src, int srcStride,
                                          uint8_t* dst, int w, int h,
                                          int x0, int y0)
{
    const __m256i mR = _mm256_set1_epi32(0xE0);
    const __m256i mG = _mm256_set1_epi32(0x1C);
    const __m256i mB = _mm256_set1_epi32(0x03);
    const __m256i sh = _mm256_setr_epi8( /* index byte of each px dword */
        0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i pm = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);

    for (int row = 0; row < h; ++row, src += srcStride, dst += w)
    {
        const uint8_t* bay = kScapBayer[(y0 + row) & 3];
        uint8_t dp[16]; /* per-pixel [B,G,R,A] offsets for x%4 = 0..3 */
        for (int k = 0; k < 4; ++k)
        {
            uint8_t m = bay[(x0 + k) & 3];
            dp[4 * k + 0] = (uint8_t)(m * 4);
            dp[4 * k + 1] = (uint8_t)(m * 2);
            dp[4 * k + 2] = (uint8_t)(m * 2);
            dp[4 * k + 3] = 0;
        }
        const __m256i dv =
            _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)dp));

        int x = 0;
        for (; x + 8 <= w; x += 8)
        {
            __m256i p = _mm256_loadu_si256((const __m256i*)(src + (size_t)x * 4));
            __m256i q = _mm256_adds_epu8(p, dv); /* dither + clamp in one op */
            __m256i r = _mm256_and_si256(_mm256_srli_epi32(q, 16), mR);
            __m256i g = _mm256_and_si256(_mm256_srli_epi32(q, 11), mG);
            __m256i b = _mm256_and_si256(_mm256_srli_epi32(q, 6), mB);
            __m256i idx = _mm256_or_si256(r, _mm256_or_si256(g, b));
            __m256i packed = _mm256_permutevar8x32_epi32(
                _mm256_shuffle_epi8(idx, sh), pm);
            _mm_storel_epi64((__m128i*)(dst + x),
                             _mm256_castsi256_si128(packed));
        }
        for (; x < w; ++x) /* tail when w is not a multiple of 8 */
            dst[x] = ScapQuant332(src + (size_t)x * 4, x0 + x, y0 + row);
    }
}
#else
#define SCAP_332_AVX2 0
static __inline int ScapQuant332DetectAvx2(void) { return 0; }
#endif

static __inline void ScapQuant332Rect(const uint8_t* src, int srcStride,
                                      uint8_t* dst, int w, int h,
                                      int x0, int y0, int useAvx2)
{
#if SCAP_332_AVX2
    if (useAvx2)
    {
        ScapQuant332RectAvx2(src, srcStride, dst, w, h, x0, y0);
        return;
    }
#else
    (void)useAvx2;
#endif
    ScapQuant332RectScalar(src, srcStride, dst, w, h, x0, y0);
}
