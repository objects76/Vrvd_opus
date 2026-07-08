/* scapenc.h - minimal Desktop Duplication -> 256-color -> zlib encoder
 * (ZipEnc-only PoC). Caller-polled, no internal threads.
 * Single-threaded: call all functions from one thread.
 */
#pragma once

#ifdef SCAPENC_EXPORTS
#define SCAPENC_API __declspec(dllexport)
#else
#define SCAPENC_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScapEnc ScapEnc;

enum
{
    SCAPENC_OK       = 0,  /* packet produced */
    SCAPENC_TIMEOUT  = 1,  /* no desktop update within timeoutMs */
    SCAPENC_NOCHANGE = 2,  /* frame acquired but zero dirty pixels (mouse-only) */
    SCAPENC_AGAIN    = 3,  /* duplication lost (mode change / UAC / secure
                              desktop); recreation pending, call again */
    SCAPENC_ERR      = -1  /* unrecoverable (no D3D11 device etc.) */
};

/* Returns NULL if D3D11 device creation fails. Duplication itself is created
 * lazily/recreated inside CaptureFrame (it can be temporarily unavailable). */
SCAPENC_API ScapEnc* ScapEnc_Create(void);
SCAPENC_API void     ScapEnc_Destroy(ScapEnc* e);

/* Primary desktop size as of the last (re)created duplication. */
SCAPENC_API int ScapEnc_GetDesktopSize(ScapEnc* e, int* w, int* h);

/* Waits up to timeoutMs for a desktop update and encodes it.
 * On SCAPENC_OK, *packet and *size reference an internal buffer valid until
 * the next ScapEnc_* call on this handle. */
SCAPENC_API int ScapEnc_CaptureFrame(ScapEnc* e, int timeoutMs,
                                     const void** packet, unsigned* size);

/* Make the next produced packet a full frame (e.g. a new viewer connected
 * and has no canvas history). Also resets the change-verification state. */
SCAPENC_API void ScapEnc_RequestFullFrame(ScapEnc* e);

#ifdef __cplusplus
}
#endif
