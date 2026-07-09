/* scap_packet.h - wire format shared by scapenc/scapdec.
 *
 * Packet = ScapFrameHdr (16 bytes) + zstd blob.
 * The encoder compresses with one long-lived zstd stream (level:
 * config.h ZSTD_LEVEL) and flushes per packet (ZSTD_e_flush), so later
 * packets reference earlier frames' payload as history - unchanged screen
 * content re-sent across frames compresses to almost nothing. Consequence:
 * packets are NOT independently decodable; the decoder feeds them in order
 * into one ZSTD_DStream. The encoder starts a new zstd frame (session
 * reset) on ScapEnc_RequestFullFrame(), which the server calls per new
 * connection, so a fresh decoder always starts at a frame boundary.
 * A blob may also be a complete standalone zstd frame (one-shot
 * ZSTD_compress); ZSTD_decompressStream handles both transparently.
 *
 * Payload (after decompression) = moveCount x ScapMoveRect
 *         + rectCount x { ScapRectHdr + w*h bytes of 8bpp palette indices },
 * row-major, no row padding. Palette is the fixed table selected by
 * scap_palette.h (scap_332dither.h by default, scap_256map.h optionally).
 *
 * Move rects are DXGI move regions forwarded as copy ops (VNC CopyRect
 * style): the decoder copies w*h pixels already on its canvas from
 * (srcX,srcY) to (x,y) - in array order, BEFORE applying pixel rects
 * (the DXGI reconstruction contract: moves first, then dirty).
 * Replaces the legacy scapEncMsg/I16Rect streaming format (no compatibility).
 */
#pragma once
#include <stdint.h>

#define SCAP_MAGIC 0x32504353u /* 'S','C','P','2' little-endian; 1 was zlib */

/* AV1 packet (runtime codec spec "av1[:i420|:i444]"): same 16-byte
 * ScapFrameHdr but the body is one raw AV1 temporal unit (libaom OBU stream)
 * for the full frame - no move/rect payload, so rectCount = moveCount = 0
 * and rawSize = OBU byte length (== packet size - header; kept for sanity
 * checking). Packets must be fed to ONE decoder in stream order; a decoder
 * can only join at a keyframe (the server creates a fresh encoder per
 * connection, so the first packet always is one; mid-session resync is
 * ScapEnc_RequestFullFrame()). Distinct magic lets the decoder dispatch the
 * codec per packet. */
#define SCAP_MAGIC_AV1 0x31414353u /* 'S','C','A','1' little-endian */

#pragma pack(push, 1)
typedef struct ScapFrameHdr
{
    uint32_t magic;     /* SCAP_MAGIC */
    uint16_t width;     /* desktop width in px */
    uint16_t height;    /* desktop height in px */
    uint16_t rectCount; /* pixel rects; rectCount + moveCount >= 1 */
    uint16_t moveCount; /* copy ops (was reserved/0, so old packets decode) */
    uint32_t rawSize;   /* payload byte length after decompression */
} ScapFrameHdr;

typedef struct ScapRectHdr
{
    uint16_t x, y, w, h; /* x+w <= width, y+h <= height, w*h > 0 */
} ScapRectHdr;

typedef struct ScapMoveRect
{
    uint16_t srcX, srcY; /* source top-left; srcX+w <= width, srcY+h <= height */
    uint16_t x, y, w, h; /* destination rect, same bounds rules as ScapRectHdr */
} ScapMoveRect;
#pragma pack(pop)
