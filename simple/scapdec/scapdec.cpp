/* scapdec.cpp - uncompress + rect blit into an 8bpp DIBSection canvas.
 * Canvas handling reduced from legacy SCapDec2/Canvas.cpp (Create/UpdateRotate0);
 * zlib streaming (icomp_stream) replaced by one-shot uncompress().
 */
#include "scapdec.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "zlib.h"
#include <stdlib.h>
#include <string.h>

struct ScapDec
{
    int      width = 0, height = 0;
    int      scan = 0;          /* DIB row stride: (width+3)&~3 */
    HDC      memDC = nullptr;
    HBITMAP  dib = nullptr;
    HGDIOBJ  oldBmp = nullptr;
    uint8_t* bits = nullptr;    /* DIBSection memory, top-down */
    uint8_t* raw = nullptr;     /* uncompress scratch */
    unsigned rawCap = 0;
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
    bmi.bmiHeader.biBitCount = 8;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biClrUsed = 256;
    for (int i = 0; i < 256; ++i)
    {
        bmi.bmiColors[i].rgbRed = kScapPal[i].r;
        bmi.bmiColors[i].rgbGreen = kScapPal[i].g;
        bmi.bmiColors[i].rgbBlue = kScapPal[i].b;
    }

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
    d->scan = (w + 3) & ~3;
    memset(bits, 1, (size_t)d->scan * h); /* palette index 1 = black */
    return 0;
}

extern "C" {

SCAPDEC_API ScapDec* ScapDec_Create(void)
{
    return new ScapDec();
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
    if (hdr.magic != SCAP_MAGIC || hdr.rectCount == 0 || hdr.width == 0 ||
        hdr.height == 0)
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

    uLongf rawLen = hdr.rawSize;
    if (uncompress(d->raw, &rawLen, (const Bytef*)packet + sizeof(hdr),
                   size - sizeof(hdr)) != Z_OK ||
        rawLen != hdr.rawSize)
        return -3;

    const uint8_t* p = d->raw;
    const uint8_t* end = d->raw + rawLen;
    RECT bounds;
    SetRectEmpty(&bounds);

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
