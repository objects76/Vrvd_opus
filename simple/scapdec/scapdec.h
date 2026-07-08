/* scapdec.h - minimal 256-color screen-stream decoder (ZipDec-only PoC).
 * Decodes packets produced by scapenc into an internal 8bpp DIB canvas.
 * Single-threaded: call all functions from one thread.
 */
#pragma once
#include <windows.h>

#ifdef SCAPDEC_EXPORTS
#define SCAPDEC_API __declspec(dllexport)
#else
#define SCAPDEC_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScapDec ScapDec;

SCAPDEC_API ScapDec* ScapDec_Create(void);
SCAPDEC_API void     ScapDec_Destroy(ScapDec* d);

/* Decodes one packet into the internal 8bpp canvas; (re)creates the canvas
 * when the header width/height changes. updatedBounds (optional) receives the
 * bounding box of updated pixels. Returns 0 on success, <0 on malformed
 * packet or decompression failure. */
SCAPDEC_API int ScapDec_DecodePacket(ScapDec* d, const void* packet,
                                     unsigned size, RECT* updatedBounds);

/* Canvas size; returns 0 and sets 0x0 until the first frame is decoded. */
SCAPDEC_API int ScapDec_GetSize(ScapDec* d, int* w, int* h);

/* BitBlt the canvas (srcRect, or the whole canvas if NULL) to hdc at (x,y).
 * Returns 0 on success, <0 if there is no canvas yet. */
SCAPDEC_API int ScapDec_Paint(ScapDec* d, HDC hdc, int x, int y,
                              const RECT* srcRect);

#ifdef __cplusplus
}
#endif
