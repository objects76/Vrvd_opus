/* scapenc.cpp - two codec paths, selected at RUNTIME by the spec string
 * passed to ScapEnc_Create() ("zstd[:level]" | "av1[:i420|:i444]", see
 * common/config.h):
 *
 * zstd: BGRA32 -> 8bpp (RGB332 + 4x4 Bayer ordered dithering)
 * -> whole-frame payload accumulation -> one streaming zstd flush -> packet.
 * The zs::StreamCompressor (common/zstd_stream.h) lives as long as the
 * encoder, so later packets reference earlier frames as history (see
 * scap_packet.h). Replaced zlib compress2. zs errors are exceptions; this
 * extern "C" boundary catches and translates.
 *
 * av1: full captured frame -> Av1Enc (libaom, Chrome Remote Desktop realtime
 * screen settings, common/av1_encoder.h) -> packet = header + one AV1
 * temporal unit. The codec's inter prediction replaces the dirty-rect/move
 * machinery, which stays zstd-only.
 */
#include "scapenc.h"
#include "dxgidup.h"
#include "dirty_verify.h"
#include "../common/config.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "../common/zstd_stream.h"
#include "../common/av1_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct ScapEnc
{
    const bool           useAv1;  /* codec chosen at Create, fixed for life */
    const bool           i444;    /* av1 only: 4:4:4 instead of 4:2:0 */
    const bool           dither;  /* zstd only: Bayer dither the 8bpp quant */
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
    zs::StreamCompressor zenc;    /* one continuing frame (zstd path) */
    Av1Enc               av1;     /* (re)inited on first frame / resize */
    unsigned             frameNo = 0;
    long long            totalMovePx = 0;

    ScapEnc(bool av1On, bool i444On, int zstdLevel, bool ditherOn)
        : useAv1(av1On), i444(i444On), dither(ditherOn),
          zenc{zs::CompressorOptions{zstdLevel}} {}
};

/* Parse a codec spec. Comes off the network (viewer hello), so unknown
 * codecs / out-of-range levels are rejected, not guessed. */
static bool ParseCodec(const char* spec, bool* av1, bool* i444, int* level,
                       bool* dither)
{
    *av1 = false;
    *i444 = SCAP_AV1_I444 != 0;
    *level = ZSTD_LEVEL;
    *dither = false;
    if (!spec || !*spec)
        spec = SCAP_CODEC_DEFAULT;
    if (strncmp(spec, "zstd", 4) == 0)
    {
        /* ":"-separated options in any order: a number = level (1..22,
         * 22 = ZSTD_maxCLevel()), "dither" = Bayer-dither the 8bpp quant. */
        const char* p = spec + 4;
        while (*p == ':')
        {
            ++p;
            const char* sep = strchr(p, ':');
            size_t n = sep ? (size_t)(sep - p) : strlen(p);
            if (n == 6 && strncmp(p, "dither", 6) == 0)
            {
                *dither = true;
            }
            else
            {
                char* end;
                long v = strtol(p, &end, 10);
                if (end != p + n || n == 0 || v < 1 || v > 22)
                    return false;
                *level = (int)v;
            }
            p += n;
        }
        return *p == '\0';
    }
    if (strncmp(spec, "av1", 3) == 0)
    {
        *av1 = true;
        const char* p = spec + 3;
        if (*p == '\0')
            return true;
        if (strcmp(p, ":i420") == 0) { *i444 = false; return true; }
        if (strcmp(p, ":i444") == 0) { *i444 = true;  return true; }
        return false;
    }
    return false;
}

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

SCAPENC_API ScapEnc* ScapEnc_Create(const char* codec)
{
    bool av1, i444, dither;
    int level;
    if (!ParseCodec(codec, &av1, &i444, &level, &dither))
        return nullptr;
    ScapEnc* e;
    try
    {
        /* zs::StreamCompressor ctor throws on failure */
        e = new ScapEnc(av1, i444, level, dither);
    }
    catch (...)
    {
        return nullptr;
    }
    if (!e->dup.Init())
    {
        delete e;
        return nullptr;
    }
    ScapQuantInit(&e->quant, e->dither);
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
    delete e;
}

SCAPENC_API void ScapEnc_RequestFullFrame(ScapEnc* e)
{
    if (!e)
        return;
    e->dup.forceFullFrame = true;
    e->prevValid = false; /* reseed prev so the full frame is sent as pixels */
    if (e->useAv1)
    {
        /* New viewer = fresh decoder: it can only join at a keyframe. */
        e->av1.RequestKeyframe();
        return;
    }
    /* New viewer = fresh decoder on the other end: start a new zstd frame so
     * its first packet is a frame boundary. */
    try
    {
        e->zenc.reset();
    }
    catch (...)
    {
        /* broken cctx: the next compressFrame() will throw and resync */
    }
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

    /* AV1 path: the codec's inter prediction replaces the dirty/move
     * machinery - encode the full frame every time DXGI delivers one. */
    if (e->useAv1)
    {
        const int w = e->dup.width, h = e->dup.height;
        if (!e->av1.ok() || e->av1.w() != w || e->av1.h() != h)
        {
            /* first frame or resolution change: fresh encoder, keyframe */
            if (!e->av1.Init(w, h, SCAP_AV1_BITRATE_KBPS, SCAP_AV1_FPS,
                             e->i444))
            {
                e->dup.Unmap();
                return SCAPENC_ERR;
            }
        }
        bool encOk = e->av1.EncodeBGRA((const uint8_t*)mapped.pData,
                                       (int)mapped.RowPitch, e->payload);
        e->dup.Unmap();
        if (!encOk)
        {
            /* this frame never reaches the peer: resync from a keyframe */
            e->av1.RequestKeyframe();
            return SCAPENC_ERR;
        }

        ScapFrameHdr hdr = {};
        hdr.magic = SCAP_MAGIC_AV1;
        hdr.width = (uint16_t)w;
        hdr.height = (uint16_t)h;
        hdr.rawSize = (uint32_t)e->payload.size(); /* OBU byte length */
        e->packet.resize(sizeof(hdr) + e->payload.size());
        memcpy(e->packet.data(), &hdr, sizeof(hdr));
        memcpy(e->packet.data() + sizeof(hdr), e->payload.data(),
               e->payload.size());

        ++e->frameNo;
        *packet = e->packet.data();
        *size = (unsigned)e->packet.size();
        return SCAPENC_OK;
    }

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

    e->packet.resize(sizeof(ScapFrameHdr)); /* blob appended after the header */
    try
    {
        e->zenc.compressFrame(e->payload.data(), e->payload.size(),
                              [e](const void* p, size_t n) {
                                  e->packet.insert(e->packet.end(),
                                                   (const uint8_t*)p,
                                                   (const uint8_t*)p + n);
                              });
    }
    catch (...)
    {
        /* The stream now holds history the client will never receive;
         * resync by starting a new zstd frame and re-sending everything. */
        try { e->zenc.reset(); } catch (...) {}
        e->dup.forceFullFrame = true;
        e->prevValid = false;
        return SCAPENC_ERR;
    }

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
        LogMoveSavings(e, (unsigned)e->packet.size());

    *packet = e->packet.data();
    *size = (unsigned)e->packet.size();
    return SCAPENC_OK;
}

} /* extern "C" */
