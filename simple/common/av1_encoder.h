/* av1_encoder.h - libaom AV1 realtime screen encoder, configured the same
 * way Chrome Remote Desktop configures it (Chromium
 * remoting/codec/webrtc_video_encoder_av1.cc): AOM_USAGE_REALTIME +
 * AV1E_SET_TUNE_CONTENT=AOM_CONTENT_SCREEN + cpu-used 11 + CBR, one-pass,
 * zero lag, microsecond timebase, cyclic-refresh AQ. Keyframes are never
 * emitted on a timer (AOM_KF_DISABLED); the first frame is forced key and
 * RequestKeyframe() forces the next one - mirrors ScapEnc_RequestFullFrame()
 * semantics for a newly attached viewer.
 *
 * Input is desktop BGRA32 (DXGI duplication layout); converted here to I420
 * (or I444 when config.h SCAP_AV1_I444 is set - full-resolution chroma,
 * profile 1) before aom_codec_encode. Error handling is by return value (no
 * exceptions), same convention as the rest of the scapenc DLL surface.
 * Requires libaom >= 3.6 (cpu-used 11); vcpkg aom:x64-windows-static.
 */
#pragma once
#include "config.h"
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <stdint.h>
#include <string.h>
#include <thread>
#include <vector>

class Av1Enc
{
public:
    Av1Enc() {}
    ~Av1Enc() { Term(); }
    Av1Enc(const Av1Enc&) = delete;
    Av1Enc& operator=(const Av1Enc&) = delete;

    /* fps only scales rate-control timing (frame duration = 1s/fps in the
     * microsecond timebase); encoding is still driven per delivered frame. */
    bool Init(int w, int h, int targetKbps, int fps)
    {
        Term();
        aom_codec_enc_cfg_t cfg;
        if (aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg,
                                         AOM_USAGE_REALTIME) != AOM_CODEC_OK)
            return false;
        cfg.g_w = (unsigned)w;
        cfg.g_h = (unsigned)h;
#if SCAP_AV1_I444
        cfg.g_profile = 1; /* 4:4:4 requires the High profile */
#endif
        cfg.g_timebase.num = 1;
        cfg.g_timebase.den = 1000000; /* microseconds, as CRD uses */
        cfg.g_pass = AOM_RC_ONE_PASS;
        cfg.g_lag_in_frames = 0;      /* zero latency */
        cfg.rc_end_usage = AOM_CBR;
        cfg.rc_target_bitrate = (unsigned)targetKbps;
        cfg.rc_min_quantizer = 10;    /* WebRTC/CRD realtime qp bounds */
        cfg.rc_max_quantizer = 63;
        cfg.kf_mode = AOM_KF_DISABLED; /* keyframes only when forced */
        unsigned hw = std::thread::hardware_concurrency();
        cfg.g_threads = hw < 1 ? 1 : (hw > 8 ? 8 : hw);

        if (aom_codec_enc_init(&ctx, aom_codec_av1_cx(), &cfg, 0) !=
            AOM_CODEC_OK)
            return false;
        ctxOpen = true;

        /* The CRD screen-share tuning. Speed 11 is the fastest realtime
         * preset (valid since libaom 3.6); AQ mode 3 = cyclic refresh;
         * palette mode is what makes flat UI content cheap. */
        if (aom_codec_control(&ctx, AOME_SET_CPUUSED, 11) != AOM_CODEC_OK ||
            aom_codec_control(&ctx, AV1E_SET_TUNE_CONTENT,
                              AOM_CONTENT_SCREEN) != AOM_CODEC_OK ||
            aom_codec_control(&ctx, AV1E_SET_AQ_MODE, 3) != AOM_CODEC_OK ||
            aom_codec_control(&ctx, AV1E_SET_ENABLE_PALETTE, 1) !=
                AOM_CODEC_OK ||
            aom_codec_control(&ctx, AV1E_SET_ROW_MT, 1) != AOM_CODEC_OK)
        {
            Term();
            return false;
        }

#if SCAP_AV1_I444
        img = aom_img_alloc(nullptr, AOM_IMG_FMT_I444, (unsigned)w,
                            (unsigned)h, 1);
#else
        img = aom_img_alloc(nullptr, AOM_IMG_FMT_I420, (unsigned)w,
                            (unsigned)h, 1);
#endif
        if (!img)
        {
            Term();
            return false;
        }
        width = w;
        height = h;
        durationUs = 1000000 / (fps > 0 ? fps : 30);
        pts = 0;
        forceKey = true; /* a fresh decoder needs a keyframe first */
        return true;
    }

    bool ok() const { return ctxOpen && img; }
    int  w() const { return width; }
    int  h() const { return height; }

    /* Force the next encoded frame to be a keyframe (new viewer attached). */
    void RequestKeyframe() { forceKey = true; }

    /* Encode one BGRA32 frame (strideBytes = bytes per row, >= width*4).
     * out receives the complete AV1 temporal unit (lag 0: every encode
     * produces exactly one frame's data). isKey reports whether it came out
     * as a keyframe. */
    bool EncodeBGRA(const uint8_t* bgra, int strideBytes,
                    std::vector<uint8_t>& out, bool* isKey = nullptr)
    {
        out.clear();
        if (isKey) *isKey = false;
        if (!ok() || !bgra || strideBytes < width * 4)
            return false;

        BgraToImg(bgra, strideBytes);

        aom_enc_frame_flags_t flags = forceKey ? AOM_EFLAG_FORCE_KF : 0;
        if (aom_codec_encode(&ctx, img, pts, (unsigned long)durationUs,
                             flags) != AOM_CODEC_OK)
            return false;
        pts += durationUs;
        forceKey = false;

        aom_codec_iter_t iter = nullptr;
        const aom_codec_cx_pkt_t* pkt;
        while ((pkt = aom_codec_get_cx_data(&ctx, &iter)) != nullptr)
        {
            if (pkt->kind != AOM_CODEC_CX_FRAME_PKT)
                continue;
            const uint8_t* d = (const uint8_t*)pkt->data.frame.buf;
            out.insert(out.end(), d, d + pkt->data.frame.sz);
            if (isKey && (pkt->data.frame.flags & AOM_FRAME_IS_KEY))
                *isKey = true;
        }
        return !out.empty();
    }

    void Term()
    {
        if (img)
        {
            aom_img_free(img);
            img = nullptr;
        }
        if (ctxOpen)
        {
            aom_codec_destroy(&ctx);
            ctxOpen = false;
        }
    }

private:
    /* BGRA -> I420/I444, BT.601 limited range (the fixed-point matrix
     * everyone uses; matches libyuv ARGBToI420 coefficients). I420 chroma is
     * averaged over each 2x2 block, clamped at odd edges; I444 keeps chroma
     * per pixel.
     * ponytail: scalar loop, ~2ms/FHD frame single-threaded. Upgrade path:
     * libyuv (already a vcpkg port) or AVX2 like scap_332dither.h. */
    void BgraToImg(const uint8_t* bgra, int stride)
    {
        uint8_t* yp = img->planes[AOM_PLANE_Y];
        uint8_t* up = img->planes[AOM_PLANE_U];
        uint8_t* vp = img->planes[AOM_PLANE_V];
        const int ys = img->stride[AOM_PLANE_Y];
        const int us = img->stride[AOM_PLANE_U];
        const int vs = img->stride[AOM_PLANE_V];

        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row = bgra + (size_t)y * stride;
            uint8_t* yrow = yp + (size_t)y * ys;
            for (int x = 0; x < width; ++x)
            {
                const uint8_t b = row[x * 4], g = row[x * 4 + 1],
                              r = row[x * 4 + 2];
                yrow[x] =
                    (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }
#if SCAP_AV1_I444
        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row = bgra + (size_t)y * stride;
            uint8_t* urow = up + (size_t)y * us;
            uint8_t* vrow = vp + (size_t)y * vs;
            for (int x = 0; x < width; ++x)
            {
                const int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                urow[x] =
                    (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                vrow[x] =
                    (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
#else
        for (int cy = 0; cy < (height + 1) / 2; ++cy)
        {
            uint8_t* urow = up + (size_t)cy * us;
            uint8_t* vrow = vp + (size_t)cy * vs;
            for (int cx = 0; cx < (width + 1) / 2; ++cx)
            {
                int rs = 0, gs = 0, bs = 0, n = 0;
                for (int dy = 0; dy < 2; ++dy)
                {
                    int py = cy * 2 + dy;
                    if (py >= height) break;
                    const uint8_t* p = bgra + (size_t)py * stride;
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        int px = cx * 2 + dx;
                        if (px >= width) break;
                        bs += p[px * 4];
                        gs += p[px * 4 + 1];
                        rs += p[px * 4 + 2];
                        ++n;
                    }
                }
                const int r = rs / n, g = gs / n, b = bs / n;
                urow[cx] =
                    (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                vrow[cx] =
                    (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
#endif
    }

    aom_codec_ctx_t ctx = {};
    aom_image_t* img = nullptr;
    bool ctxOpen = false;
    bool forceKey = true;
    int width = 0, height = 0;
    long long durationUs = 33333;
    aom_codec_pts_t pts = 0;
};
