/* config.h - build-time codec selection for scapenc/scapdec.
 *
 * USE_AV1 = 1 : AV1 video codec - libaom realtime screen encoder (the same
 *               configuration Chrome Remote Desktop uses, see av1_encoder.h)
 *               and dav1d decoder (as WebRTC/CRD, see av1_decoder.h).
 *               Packets carry raw AV1 temporal units (SCAP_MAGIC_AV1).
 * USE_AV1 = 0 : the original PoC path - 8bpp palette quantization + one
 *               continuing streaming-zstd frame (SCAP_MAGIC).
 *
 * Both ends of a connection must be built with the same value: the wire
 * formats are disjoint (distinct magics, so a mismatch fails loudly instead
 * of rendering garbage). viewer_x11 (Linux) only implements the zstd path
 * so far - keep USE_AV1 0 when testing against it.
 */
#pragma once

#define USE_AV1 1

#if USE_AV1
/* Chroma subsampling for the AV1 encoder input: 0 = I420 (4:2:0, what
 * CRD/WebRTC use - chroma at half resolution, colored text edges soften),
 * 1 = I444 (4:4:4, full-resolution chroma, sharper UI colors, bigger
 * packets; uses AV1 profile 1). The decoder reads the actual layout from
 * the bitstream, so it follows automatically. */
#define SCAP_AV1_I444 0
#endif

/* Human-readable codec name for title bars / logs. */
#if !USE_AV1
#define SCAP_CODEC_NAME "zstd"
#elif SCAP_AV1_I444
#define SCAP_CODEC_NAME "AV1/I444"
#else
#define SCAP_CODEC_NAME "AV1/I420"
#endif

#if USE_AV1
/* Fixed CBR target for the AV1 encoder. CRD drives this from the WebRTC
 * bandwidth estimator; this PoC has no estimator, so pick a static budget.
 * ponytail: 8 Mbps is comfortable for FHD screen content; upgrade path is
 * feeding a measured send-rate back into Av1Enc (aom cfg.rc_target_bitrate
 * can be updated between frames). */
#define SCAP_AV1_BITRATE_KBPS 8000
/* Nominal frame rate for rate control (frame duration = 1s/fps). */
#define SCAP_AV1_FPS 30
#endif
