/* scapenc.cpp - BGRA32 -> 8bpp (fixed palette LUT, legacy Trans32_8 semantics)
 * -> whole-frame payload accumulation -> one compress2() -> packet.
 * Replaces the legacy per-tile streaming deflate (ZipEnc/ocomp_stream/rszip).
 */
#include "scapenc.h"
#include "dxgidup.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "zlib.h"
#include <string.h>
#include <vector>

struct ScapEnc
{
    DxgiDup              dup;
    uint8_t              lut[32768];
    std::vector<RECT>    dirty;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moves;
    std::vector<uint8_t> payload; /* move rects + rect headers + 8bpp pixels */
    std::vector<uint8_t> packet;  /* ScapFrameHdr + compress2 blob */
};

extern "C" {

SCAPENC_API ScapEnc* ScapEnc_Create(void)
{
    ScapEnc* e = new ScapEnc();
    if (!e->dup.Init())
    {
        delete e;
        return nullptr;
    }
    ScapBuildLut(e->lut);
    return e;
}

SCAPENC_API void ScapEnc_Destroy(ScapEnc* e)
{
    if (!e)
        return;
    e->dup.Term();
    delete e;
}

SCAPENC_API int ScapEnc_GetDesktopSize(ScapEnc* e, int* w, int* h)
{
    if (w) *w = e ? e->dup.width : 0;
    if (h) *h = e ? e->dup.height : 0;
    return (e && e->dup.width) ? 0 : -1;
}

SCAPENC_API int ScapEnc_CaptureFrame(ScapEnc* e, int timeoutMs,
                                     const void** packet, unsigned* size)
{
    if (packet) *packet = nullptr;
    if (size) *size = 0;
    if (!e || !packet || !size)
        return SCAPENC_ERR;

    switch (e->dup.Acquire(timeoutMs, e->dirty, e->moves))
    {
    case DxgiDup::FRAME:    break;
    case DxgiDup::TIMEOUT:  return SCAPENC_TIMEOUT;
    case DxgiDup::NOCHANGE: return SCAPENC_NOCHANGE;
    case DxgiDup::AGAIN:    return SCAPENC_AGAIN;
    default:                return SCAPENC_ERR;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!e->dup.Map(&mapped))
        return SCAPENC_ERR;

    /* Accumulate the whole frame's payload in memory, compress once at the
     * end (one-shot compress2 style, per project requirement). */
    e->payload.clear();
    for (const DXGI_OUTDUPL_MOVE_RECT& m : e->moves)
    {
        ScapMoveRect mv = { (uint16_t)m.SourcePoint.x,
                            (uint16_t)m.SourcePoint.y,
                            (uint16_t)m.DestinationRect.left,
                            (uint16_t)m.DestinationRect.top,
                            (uint16_t)(m.DestinationRect.right -
                                       m.DestinationRect.left),
                            (uint16_t)(m.DestinationRect.bottom -
                                       m.DestinationRect.top) };
        const uint8_t* pmv = (const uint8_t*)&mv;
        e->payload.insert(e->payload.end(), pmv, pmv + sizeof(mv));
    }
    for (const RECT& r : e->dirty)
    {
        ScapRectHdr rh = { (uint16_t)r.left, (uint16_t)r.top,
                           (uint16_t)(r.right - r.left),
                           (uint16_t)(r.bottom - r.top) };
        const uint8_t* prh = (const uint8_t*)&rh;
        e->payload.insert(e->payload.end(), prh, prh + sizeof(rh));

        size_t at = e->payload.size();
        e->payload.resize(at + (size_t)rh.w * rh.h);
        uint8_t* dst = e->payload.data() + at;
        const uint8_t* src = (const uint8_t*)mapped.pData +
                             (size_t)r.top * mapped.RowPitch + (size_t)r.left * 4;
        for (int row = 0; row < rh.h; ++row, src += mapped.RowPitch)
        {
            const uint8_t* px = src;
            for (int x = 0; x < rh.w; ++x, px += 4)
                *dst++ = e->lut[ScapC32_15(px)];
        }
    }
    e->dup.Unmap();

    uLongf zLen = compressBound((uLong)e->payload.size());
    e->packet.resize(sizeof(ScapFrameHdr) + zLen);
    if (compress2(e->packet.data() + sizeof(ScapFrameHdr), &zLen,
                  e->payload.data(), (uLong)e->payload.size(),
                  Z_DEFAULT_COMPRESSION) != Z_OK)
        return SCAPENC_ERR;

    ScapFrameHdr hdr = {};
    hdr.magic = SCAP_MAGIC;
    hdr.width = (uint16_t)e->dup.width;
    hdr.height = (uint16_t)e->dup.height;
    hdr.rectCount = (uint16_t)e->dirty.size();
    hdr.moveCount = (uint16_t)e->moves.size();
    hdr.rawSize = (uint32_t)e->payload.size();
    memcpy(e->packet.data(), &hdr, sizeof(hdr));

    *packet = e->packet.data();
    *size = (unsigned)(sizeof(ScapFrameHdr) + zLen);
    return SCAPENC_OK;
}

} /* extern "C" */
