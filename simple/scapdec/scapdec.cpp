/* scapdec.cpp - two codec paths, selected by common/config.h USE_AV1:
 *
 * zstd (USE_AV1=0): streaming decompress + rect blit into an 8bpp
 * DIBSection canvas. Canvas handling reduced from legacy SCapDec2/Canvas.cpp
 * (Create/UpdateRotate0). The zs::StreamDecompressor (common/zstd_stream.h)
 * lives as long as the decoder and packets must be fed in stream order: the
 * encoder flushes (not ends) its zstd frame per packet, so a packet's blob
 * can reference earlier packets' payload as history (see scap_packet.h).
 * zs errors are exceptions; this extern "C" boundary catches and translates.
 *
 * AV1 (USE_AV1=1): each packet is one AV1 temporal unit; Av1Dec (dav1d,
 * common/av1_decoder.h) decodes it into a 32bpp BGRA DIBSection canvas.
 * Same stream-order requirement; a fresh decoder joins at the keyframe the
 * server forces per connection.
 */
#include "scapdec.h"
#include "../common/config.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "../common/zstd_stream.h"
#if USE_AV1
#include "../common/av1_decoder.h"
#endif
#include <stdlib.h>
#include <string.h>

struct ScapDec
{
    int      width = 0, height = 0;
    int      scan = 0;          /* DIB row stride in bytes */
    HDC      memDC = nullptr;
    HBITMAP  dib = nullptr;
    HGDIOBJ  oldBmp = nullptr;
    uint8_t* bits = nullptr;    /* DIBSection memory, top-down */
    uint8_t* raw = nullptr;     /* decompress scratch */
    unsigned rawCap = 0;
    zs::StreamDecompressor zdec;
#if USE_AV1
    Av1Dec   av1;               /* dav1d; Init() in ScapDec_Create */
#endif
};

static void DestroyCanvas(ScapDec* d)
{
    if (d->memDC)
    {
        SelectObject(d->memDC, d->oldBmp);
        DeleteObject(d->dib);
        DeleteDC(d->memDC);
    }
    d->memDC = nullptr;
    d->dib = nullptr;
    d->oldBmp = nullptr;
    d->bits = nullptr;
    d->width = d->height = d->scan = 0;
}

static int CreateCanvas(ScapDec* d, int w, int h)
{
    DestroyCanvas(d);

    struct
    {
        BITMAPINFOHEADER bmiHeader;
        RGBQUAD bmiColors[256];
    } bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biCompression = BI_RGB;
#if USE_AV1
    bmi.bmiHeader.biBitCount = 32; /* BGRA straight from the AV1 decoder */
#else
    bmi.bmiHeader.biBitCount = 8;
    bmi.bmiHeader.biClrUsed = 256;
    for (int i = 0; i < 256; ++i)
    {
        bmi.bmiColors[i].rgbRed = kScapPal[i].r;
        bmi.bmiColors[i].rgbGreen = kScapPal[i].g;
        bmi.bmiColors[i].rgbBlue = kScapPal[i].b;
    }
#endif

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, (BITMAPINFO*)&bmi, DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    if (!dib)
        return -1;

    HDC memDC = CreateCompatibleDC(nullptr);
    if (!memDC)
    {
        DeleteObject(dib);
        return -1;
    }

    d->oldBmp = SelectObject(memDC, dib);
    d->memDC = memDC;
    d->dib = dib;
    d->bits = (uint8_t*)bits;
    d->width = w;
    d->height = h;
#if USE_AV1
    d->scan = w * 4;
#else
    d->scan = (w + 3) & ~3;
#endif
    memset(bits, 0, (size_t)d->scan * h); /* zero = black in both formats */
    return 0;
}

extern "C" {

SCAPDEC_API ScapDec* ScapDec_Create(void)
{
    ScapDec* d;
    try
    {
        d = new ScapDec(); /* zs::StreamDecompressor ctor throws on failure */
    }
    catch (...)
    {
        return nullptr;
    }
#if USE_AV1
    if (!d->av1.Init()) /* dav1d_open failure */
    {
        delete d;
        return nullptr;
    }
#endif
    return d;
}

SCAPDEC_API void ScapDec_Destroy(ScapDec* d)
{
    if (!d)
        return;
    DestroyCanvas(d);
    free(d->raw);
    delete d;
}

SCAPDEC_API int ScapDec_DecodePacket(ScapDec* d, const void* packet,
                                     unsigned size, RECT* updatedBounds)
{
    if (updatedBounds)
        SetRectEmpty(updatedBounds);
    if (!d || !packet || size < sizeof(ScapFrameHdr))
        return -1;

    ScapFrameHdr hdr;
    memcpy(&hdr, packet, sizeof(hdr));
#if USE_AV1
    /* AV1 packet: header + one temporal unit; the decoded picture is always
     * a full frame, so the updated bounds are the whole canvas. */
    if (hdr.magic != SCAP_MAGIC_AV1 || hdr.width == 0 || hdr.height == 0 ||
        hdr.rawSize != size - sizeof(hdr))
        return -1;

    if (hdr.width != d->width || hdr.height != d->height)
        if (CreateCanvas(d, hdr.width, hdr.height) != 0)
            return -2;

    if (!d->av1.DecodeToBGRA((const uint8_t*)packet + sizeof(hdr),
                             size - sizeof(hdr), d->bits, d->scan,
                             d->width, d->height))
        return -3;

    if (updatedBounds)
    {
        RECT full = { 0, 0, d->width, d->height };
        *updatedBounds = full;
    }
    return 0;
#else
    if (hdr.magic != SCAP_MAGIC || hdr.rectCount + hdr.moveCount == 0 ||
        hdr.width == 0 || hdr.height == 0)
        return -1;

    if (hdr.width != d->width || hdr.height != d->height)
        if (CreateCanvas(d, hdr.width, hdr.height) != 0)
            return -2;

    if (d->rawCap < hdr.rawSize)
    {
        uint8_t* p = (uint8_t*)realloc(d->raw, hdr.rawSize);
        if (!p)
            return -2;
        d->raw = p;
        d->rawCap = hdr.rawSize;
    }

    /* Decode the blob through the sink into raw, expecting exactly rawSize
     * bytes out (the encoder flushed everything for this packet). Excess
     * output means the packet lies about rawSize: clamp the copy and fail. */
    size_t got = 0;
    bool over = false;
    try
    {
        d->zdec.decompress((const uint8_t*)packet + sizeof(hdr),
                           size - sizeof(hdr),
                           [d, &hdr, &got, &over](const void* p, size_t n) {
                               if (got + n > hdr.rawSize)
                               {
                                   over = true;
                                   n = hdr.rawSize - got;
                               }
                               memcpy(d->raw + got, p, n);
                               got += n;
                           });
    }
    catch (...)
    {
        return -3;
    }
    if (over || got != hdr.rawSize)
        return -3;

    const uint8_t* p = d->raw;
    const uint8_t* end = d->raw + hdr.rawSize;
    RECT bounds;
    SetRectEmpty(&bounds);

    /* Copy ops first (the DXGI/CopyRect contract: moves before dirty), in
     * array order. src and dst may overlap (scroll), so pick the row walk
     * direction like memmove does; within a row memmove handles overlap. */
    for (unsigned i = 0; i < hdr.moveCount; ++i)
    {
        ScapMoveRect mv;
        if (end - p < (ptrdiff_t)sizeof(mv))
            return -4;
        memcpy(&mv, p, sizeof(mv));
        p += sizeof(mv);

        if (mv.w == 0 || mv.h == 0 || mv.x + mv.w > d->width ||
            mv.y + mv.h > d->height || mv.srcX + mv.w > d->width ||
            mv.srcY + mv.h > d->height)
            return -4;

        uint8_t* src = d->bits + (size_t)mv.srcY * d->scan + mv.srcX;
        uint8_t* dst = d->bits + (size_t)mv.y * d->scan + mv.x;
        if (mv.y > mv.srcY) /* downward move: walk rows bottom-up */
        {
            src += (size_t)(mv.h - 1) * d->scan;
            dst += (size_t)(mv.h - 1) * d->scan;
            for (int row = 0; row < mv.h; ++row, src -= d->scan, dst -= d->scan)
                memmove(dst, src, mv.w);
        }
        else
        {
            for (int row = 0; row < mv.h; ++row, src += d->scan, dst += d->scan)
                memmove(dst, src, mv.w);
        }

        RECT r = { mv.x, mv.y, mv.x + mv.w, mv.y + mv.h };
        UnionRect(&bounds, &bounds, &r);
    }

    for (unsigned i = 0; i < hdr.rectCount; ++i)
    {
        ScapRectHdr rc;
        if (end - p < (ptrdiff_t)sizeof(rc))
            return -4;
        memcpy(&rc, p, sizeof(rc));
        p += sizeof(rc);

        size_t pixels = (size_t)rc.w * rc.h;
        if (rc.w == 0 || rc.h == 0 || rc.x + rc.w > d->width ||
            rc.y + rc.h > d->height || (size_t)(end - p) < pixels)
            return -4;

        uint8_t* dst = d->bits + (size_t)rc.y * d->scan + rc.x;
        for (int row = 0; row < rc.h; ++row, dst += d->scan, p += rc.w)
            memcpy(dst, p, rc.w);

        RECT r = { rc.x, rc.y, rc.x + rc.w, rc.y + rc.h };
        UnionRect(&bounds, &bounds, &r);
    }

    if (updatedBounds)
        *updatedBounds = bounds;
    return 0;
#endif /* USE_AV1 */
}

SCAPDEC_API int ScapDec_GetSize(ScapDec* d, int* w, int* h)
{
    if (w) *w = d ? d->width : 0;
    if (h) *h = d ? d->height : 0;
    return (d && d->width) ? 0 : -1;
}

SCAPDEC_API int ScapDec_Paint(ScapDec* d, HDC hdc, int x, int y,
                              const RECT* srcRect)
{
    if (!d || !d->memDC)
        return -1;
    RECT r = { 0, 0, d->width, d->height };
    if (srcRect)
        r = *srcRect;
    BitBlt(hdc, x + r.left, y + r.top, r.right - r.left, r.bottom - r.top,
           d->memDC, r.left, r.top, SRCCOPY);
    return 0;
}

} /* extern "C" */
