/* config.h - DEFAULT codec settings for scapenc/scapdec.
 *
 * The codec is chosen at RUNTIME per connection: the viewer sends a
 * ScapHello (scap_stream.h) carrying a codec spec string right after
 * connect, and the server passes it to ScapEnc_Create(). Spec grammar:
 *
 *   "zstd[:level]"      8bpp palette + streaming zstd (level 1..22)
 *   "av1[:i420|:i444]"  libaom AV1 (i420 = 4:2:0 chroma, i444 = 4:4:4)
 *
 * scapdec needs no spec: packets are self-describing (distinct magics
 * per codec, see scap_packet.h), so the decoder follows whatever arrives.
 * The values below are only the defaults used when a spec omits a field.
 */
#pragma once

/* Spec used when ScapEnc_Create() gets NULL/"" (test.exe, empty hello),
 * and the viewer's --codec default. */
#define SCAP_CODEC_DEFAULT "zstd:6"

/* zstd level for a bare "zstd" spec. Encoder-side only - the zstd format is
 * self-describing, so the decoder needs no matching value. */
#define ZSTD_LEVEL 6

/* Chroma subsampling for a bare "av1" spec: 0 = I420 (4:2:0, what CRD/WebRTC
 * use - chroma at half resolution, colored text edges soften), 1 = I444
 * (4:4:4, full-resolution chroma, sharper UI colors, bigger packets; uses
 * AV1 profile 1). The decoder reads the actual layout from the bitstream. */
#define SCAP_AV1_I444 1

/* Fixed CBR target for the AV1 encoder. CRD drives this from the WebRTC
 * bandwidth estimator; this PoC has no estimator, so pick a static budget.
 * ponytail: 8 Mbps is comfortable for FHD screen content; upgrade path is
 * feeding a measured send-rate back into Av1Enc (aom cfg.rc_target_bitrate
 * can be updated between frames). */
#define SCAP_AV1_BITRATE_KBPS 8000
/* Nominal frame rate for rate control (frame duration = 1s/fps). */
#define SCAP_AV1_FPS 30

/* viewer_x11 (Linux) only: its inline decoder is still selected at BUILD
 * time. USE_AV1 picks which decode path is compiled in, and SCAP_CODEC_SPEC
 * is the spec it sends in its hello (it can only decode what it was built
 * for). The Windows binaries ignore USE_AV1 entirely. */
#define USE_AV1 1

#if !USE_AV1
#define SCAP_CODEC_SPEC "zstd"
#define SCAP_CODEC_NAME "zstd"
#elif SCAP_AV1_I444
#define SCAP_CODEC_SPEC "av1:i444"
#define SCAP_CODEC_NAME "AV1/I444"
#else
#define SCAP_CODEC_SPEC "av1:i420"
#define SCAP_CODEC_NAME "AV1/I420"
#endif
