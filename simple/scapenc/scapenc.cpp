/* scapenc.cpp - BGRA32 -> 8bpp (RGB332 + 4x4 Bayer ordered dithering)
 * -> whole-frame payload accumulation -> zstd streaming flush -> packet.
 * One ZSTD_CCtx lives as long as the encoder; each frame is compressed with
 * ZSTD_e_flush so the stream continues across packets and later frames
 * reference earlier ones as history (see scap_packet.h). Level 3 =
 * ZSTD_CLEVEL_DEFAULT. Replaced zlib compress2 (one-shot, no history).
 */
#include "scapenc.h"
#include "dxgidup.h"
#include "dirty_verify.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "zstd.h"
#include <stdio.h>
#include <string.h>
#include <vector>

struct ScapEnc
{
    DxgiDup              dup;
    ScapQuant            quant;
    std::vector<RECT>    dirty;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moves;
    std::vector<uint8_t> prev;    /* BGRA mirror of what the client has */
    bool                 prevValid = false;
    std::vector<RECT>    vDirty;  /* ScapVerifyFrame output scratch */
    std::vector<DXGI_OUTDUPL_MOVE_RECT> vMoves;
    std::vector<uint8_t> payload; /* move rects + rect headers + 8bpp pixels */
    std::vector<uint8_t> packet;  /* ScapFrameHdr + zstd blob */
    ZSTD_CCtx*           cctx = nullptr;
    unsigned             frameNo = 0;
    long long            totalMovePx = 0;
};

/* moveRect.log: per frame that carries moves, how many pixels the old
 * move-as-dirty path would have re-sent vs the 12-byte copy ops actually
 * sent. Pixel counts are pre-compression 8bpp bytes.
 * ponytail: written to the workspace root, assumed to be 4 components above
 * the dll (simple\bin\<plat>\<cfg>); if the dll is deployed elsewhere the
 * log follows the same relative ascent. */
extern "C" IMAGE_DOS_HEADER __ImageBase;

static FILE* OpenMoveLog(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA((HINSTANCE)&__ImageBase, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    char* slash = strrchr(path, '\\');
    size_t dir = slash ? (slash - path + 1) : 0;
    static const char rel[] = "..\\..\\..\\..\\moveRect.log";
    if (dir + sizeof(rel) > MAX_PATH)
        return nullptr;
    memcpy(path + dir, rel, sizeof(rel));
    FILE* f = nullptr;
    fopen_s(&f, path, "a");
    return f;
}

static void LogMoveSavings(ScapEnc* e, unsigned pktSize)
{
    long long movePx = 0;
    for (const DXGI_OUTDUPL_MOVE_RECT& m : e->moves)
        movePx += (long long)(m.DestinationRect.right - m.DestinationRect.left) *
                  (m.DestinationRect.bottom - m.DestinationRect.top);
    long long dirtyPx = 0;
    for (const RECT& r : e->dirty)
        dirtyPx += (long long)(r.right - r.left) * (r.bottom - r.top);
    e->totalMovePx += movePx;

    FILE* f = OpenMoveLog();
    if (!f)
        return;
    fprintf(f, "frame %u: rawMoves=%d forwarded=%zu savedPx=%lld"
               " (sent %zu B of copy ops instead)"
               " dirtyRects=%zu dirtyPx=%lld pkt=%u B | session savedPx=%lld\n",
            e->frameNo, e->dup.lastRawMoves, e->moves.size(), movePx,
            e->moves.size() * sizeof(ScapMoveRect),
            e->dirty.size(), dirtyPx, pktSize, e->totalMovePx);
    fclose(f);
}

/* Session totals: written on destroy. rawMoves == 0 across a session with
 * scroll activity means this machine's DXGI simply never reports moves;
 * droppedByFullFrame > 0 means moves arrived but our full-frame fallbacks
 * (forceFullFrame / AccumulatedFrames>1 / >60% dirty) discarded them. */
static void LogSessionSummary(ScapEnc* e)
{
    FILE* f = OpenMoveLog();
    if (!f)
        return;
    fprintf(f, "---- session end: frames=%lld rawMoves=%lld forwarded=%lld"
               " unmovedDemoted=%lld droppedByFullFrame=%lld savedPx=%lld ----\n",
            e->dup.statFrames, e->dup.statRawMoves, e->dup.statForwarded,
            e->dup.statUnmoved, e->dup.statDroppedFull, e->totalMovePx);
    fclose(f);
}

extern "C" {

SCAPENC_API ScapEnc* ScapEnc_Create(void)
{
    ScapEnc* e = new ScapEnc();
    e->cctx = ZSTD_createCCtx();
    if (!e->cctx || !e->dup.Init())
    {
        ZSTD_freeCCtx(e->cctx);
        delete e;
        return nullptr;
    }
    ZSTD_CCtx_setParameter(e->cctx, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    /* windowLog 21 = 2MiB history ~= one FHD 8bpp frame: the previous frame
     * acts as the dictionary for the next one, which is where the streaming
     * gain comes from. LDM stays off on the level-3 realtime path. Bump to
     * 23 for 4K targets (decoders already accept up to windowLogMax 24).
     * Buffering note: input is the whole frame payload and output is one
     * ZSTD_compressBound-sized buffer on purpose - the 128KiB-in/32-64KiB-out
     * chunking advice applies to socket-sink pipelines; our length-prefixed
     * packets need the complete blob before sending, so chunking would only
     * add loop iterations. See research/zstd_stream.hpp and
     * research/2026-07-08-Zstd_compression_ratio_and_speed_comparison.md. */
    ZSTD_CCtx_setParameter(e->cctx, ZSTD_c_windowLog, 21);
    ScapQuantInit(&e->quant);
    if (FILE* f = OpenMoveLog())
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "---- session start %04d-%02d-%02d %02d:%02d:%02d ----\n",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fclose(f);
    }
    return e;
}

SCAPENC_API void ScapEnc_Destroy(ScapEnc* e)
{
    if (!e)
        return;
    LogSessionSummary(e);
    e->dup.Term();
    ZSTD_freeCCtx(e->cctx);
    delete e;
}

SCAPENC_API void ScapEnc_RequestFullFrame(ScapEnc* e)
{
    if (!e)
        return;
    e->dup.forceFullFrame = true;
    e->prevValid = false; /* reseed prev so the full frame is sent as pixels */
    /* New viewer = fresh ZSTD_DStream on the other end: end the current zstd
     * frame and start a new one so its first packet is a frame boundary.
     * Parameters (level) survive a session-only reset. */
    ZSTD_CCtx_reset(e->cctx, ZSTD_reset_session_only);
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

    /* Refine DXGI metadata down to the pixels that actually changed by
     * comparing against prev (a BGRA mirror of the client's canvas). On the
     * first frame / after ScapEnc_RequestFullFrame / on resize there is no
     * valid prev, so send the rects as-is (a full-frame rect) and seed prev. */
    const uint8_t* cur = (const uint8_t*)mapped.pData;
    const int w = e->dup.width, h = e->dup.height;
    const size_t prevStride = (size_t)w * 4;
    if (!e->prevValid || e->prev.size() != prevStride * h)
    {
        e->prev.resize(prevStride * h);
        for (int y = 0; y < h; ++y)
            memcpy(e->prev.data() + y * prevStride, cur + (size_t)y * mapped.RowPitch,
                   prevStride);
        e->prevValid = true;
    }
    else
    {
        ScapVerifyFrame(e->prev.data(), (int)prevStride, cur, (int)mapped.RowPitch,
                        w, h, e->moves.data(), (int)e->moves.size(),
                        e->dirty.data(), (int)e->dirty.size(), e->vMoves, e->vDirty);
        e->moves.swap(e->vMoves);
        e->dirty.swap(e->vDirty);
        if (e->moves.empty() && e->dirty.empty())
        {
            e->dup.Unmap(); /* over-reported frame: nothing really changed */
            return SCAPENC_NOCHANGE;
        }
    }

    /* Accumulate the whole frame's payload in memory, compress once at the
     * end with a single streaming flush. */
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
        /* absolute frame coords keep the dither pattern (if the backend has
         * one) aligned across dirty rects: unchanged pixels re-encode to the
         * same index */
        ScapQuantRect(&e->quant, src, (int)mapped.RowPitch, dst,
                      rh.w, rh.h, r.left, r.top);
    }
    e->dup.Unmap();

    /* ZSTD_compressBound covers the worst case for the payload plus block
     * epilogues, so one flush call normally completes; the grow-and-retry
     * loop is a safety net, not the expected path. */
    size_t zCap = ZSTD_compressBound(e->payload.size());
    e->packet.resize(sizeof(ScapFrameHdr) + zCap);
    ZSTD_inBuffer  zin = { e->payload.data(), e->payload.size(), 0 };
    ZSTD_outBuffer zout = { e->packet.data() + sizeof(ScapFrameHdr), zCap, 0 };
    for (;;)
    {
        size_t rem = ZSTD_compressStream2(e->cctx, &zout, &zin, ZSTD_e_flush);
        if (ZSTD_isError(rem))
        {
            /* The stream now holds history the client will never receive;
             * resync by starting a new zstd frame and re-sending everything. */
            ZSTD_CCtx_reset(e->cctx, ZSTD_reset_session_only);
            e->dup.forceFullFrame = true;
            e->prevValid = false;
            return SCAPENC_ERR;
        }
        if (rem == 0 && zin.pos == zin.size)
            break;
        zCap += zCap / 2 + 64;
        e->packet.resize(sizeof(ScapFrameHdr) + zCap);
        zout.dst = e->packet.data() + sizeof(ScapFrameHdr);
        zout.size = zCap;
    }
    size_t zLen = zout.pos;

    ScapFrameHdr hdr = {};
    hdr.magic = SCAP_MAGIC;
    hdr.width = (uint16_t)e->dup.width;
    hdr.height = (uint16_t)e->dup.height;
    hdr.rectCount = (uint16_t)e->dirty.size();
    hdr.moveCount = (uint16_t)e->moves.size();
    hdr.rawSize = (uint32_t)e->payload.size();
    memcpy(e->packet.data(), &hdr, sizeof(hdr));

    ++e->frameNo;
    if (e->dup.lastRawMoves > 0 || !e->moves.empty())
        LogMoveSavings(e, (unsigned)(sizeof(ScapFrameHdr) + zLen));

    *packet = e->packet.data();
    *size = (unsigned)(sizeof(ScapFrameHdr) + zLen);
    return SCAPENC_OK;
}

} /* extern "C" */
