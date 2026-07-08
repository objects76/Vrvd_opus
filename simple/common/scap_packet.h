/* scap_packet.h - wire format shared by scapenc/scapdec.
 *
 * Packet = ScapFrameHdr (16 bytes) + zlib blob (compress2 of the payload).
 * Payload = rectCount x { ScapRectHdr + w*h bytes of 8bpp palette indices },
 * row-major, no row padding. Palette is the fixed table in scap_palette.h.
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
    uint16_t rectCount; /* >= 1 */
    uint16_t reserved;  /* 0 */
    uint32_t rawSize;   /* payload byte length after uncompress */
} ScapFrameHdr;

typedef struct ScapRectHdr
{
    uint16_t x, y, w, h; /* x+w <= width, y+h <= height, w*h > 0 */
} ScapRectHdr;
#pragma pack(pop)
