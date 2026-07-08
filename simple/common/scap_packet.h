/* scap_packet.h - wire format shared by scapenc/scapdec.
 *
 * Packet = ScapFrameHdr (16 bytes) + zlib blob (compress2 of the payload).
 * Payload = moveCount x ScapMoveRect
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

#define SCAP_MAGIC 0x31504353u /* 'S','C','P','1' little-endian */

#pragma pack(push, 1)
typedef struct ScapFrameHdr
{
    uint32_t magic;     /* SCAP_MAGIC */
    uint16_t width;     /* desktop width in px */
    uint16_t height;    /* desktop height in px */
    uint16_t rectCount; /* pixel rects; rectCount + moveCount >= 1 */
    uint16_t moveCount; /* copy ops (was reserved/0, so old packets decode) */
    uint32_t rawSize;   /* payload byte length after uncompress */
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
