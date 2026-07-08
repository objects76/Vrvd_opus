/* av1_decoder.h - dav1d AV1 decoder, configured as WebRTC (and therefore
 * Chrome Remote Desktop) configures it in
 * modules/video_coding/codecs/av1/dav1d_decoder.cc: default settings with
 * max_frame_delay = 1, which puts dav1d in low-delay mode - every complete
 * temporal unit sent yields its picture immediately (no frame-threading
 * pipeline latency).
 *
 * One packet from scapenc = one temporal unit = one full BGRA frame out.
 * Feed packets in stream order to ONE Av1Dec; a fresh decoder joins at a
 * keyframe (the server forces one per new connection). Error handling is by
 * return value, same convention as the rest of the scapdec DLL surface.
 * Requires dav1d; vcpkg dav1d:x64-windows-static.
 */
#pragma once
#include <dav1d/dav1d.h>
#include <errno.h> /* EAGAIN for DAV1D_ERR */
#include <stdint.h>
#include <string.h>

class Av1Dec
{
public:
    Av1Dec() {}
    ~Av1Dec() { Term(); }
    Av1Dec(const Av1Dec&) = delete;
    Av1Dec& operator=(const Av1Dec&) = delete;

    bool Init()
    {
        Term();
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.max_frame_delay = 1; /* low latency, as WebRTC dav1d_decoder.cc */
        return dav1d_open(&ctx, &s) == 0;
    }

    bool ok() const { return ctx != nullptr; }

    /* Decode one temporal unit and write the picture as BGRA32 into dst
     * (top-down, dstStride bytes per row). The decoded picture must be
     * 8-bit I420 of exactly w x h (what Av1Enc produces); anything else is
     * an error. Returns false on decode failure - the stream is then out of
     * sync: drop the connection / request a keyframe. */
    bool DecodeToBGRA(const void* obu, size_t size, uint8_t* dst,
                      int dstStride, int w, int h)
    {
        if (!ctx || !obu || size == 0)
            return false;

        Dav1dData data = {};
        uint8_t* buf = dav1d_data_create(&data, size);
        if (!buf)
            return false;
        memcpy(buf, obu, size);

        /* Send the whole TU, draining pictures whenever dav1d asks us to
         * (DAV1D_ERR(EAGAIN)); then drain what's left. With
         * max_frame_delay=1 this is one send + one get in practice. */
        Dav1dPicture pic = {};
        bool got = false;
        while (data.sz > 0)
        {
            int sr = dav1d_send_data(ctx, &data);
            if (sr != 0 && sr != DAV1D_ERR(EAGAIN))
            {
                dav1d_data_unref(&data);
                goto fail;
            }
            if (!TakePicture(pic, got) && sr != 0)
            {
                /* send stalled but no picture available: broken stream */
                dav1d_data_unref(&data);
                goto fail;
            }
        }
        while (TakePicture(pic, got))
            ;
        if (!got)
            return false;

        if (pic.p.w != w || pic.p.h != h || pic.p.bpc != 8 ||
            (pic.p.layout != DAV1D_PIXEL_LAYOUT_I420 &&
             pic.p.layout != DAV1D_PIXEL_LAYOUT_I444))
        {
            dav1d_picture_unref(&pic);
            return false;
        }
        YuvToBgra(pic, dst, dstStride, w, h);
        dav1d_picture_unref(&pic);
        return true;

    fail:
        if (got)
            dav1d_picture_unref(&pic);
        return false;
    }

    void Term()
    {
        if (ctx)
            dav1d_close(&ctx); /* sets ctx to nullptr */
    }

private:
    /* Fetch the next picture if one is ready; keep only the newest. */
    bool TakePicture(Dav1dPicture& pic, bool& got)
    {
        Dav1dPicture p = {};
        if (dav1d_get_picture(ctx, &p) != 0)
            return false;
        if (got)
            dav1d_picture_unref(&pic);
        pic = p;
        got = true;
        return true;
    }

    /* I420/I444 -> BGRA, BT.601 limited range (inverse of Av1Enc::BgraToImg,
     * same fixed-point matrix as libyuv). I420 chroma is nearest (x/2, y/2);
     * I444 chroma is per pixel (ss shift 0).
     * ponytail: scalar loop; upgrade path libyuv I420ToARGB. */
    static void YuvToBgra(const Dav1dPicture& pic, uint8_t* dst,
                          int dstStride, int w, int h)
    {
        const uint8_t* yp = (const uint8_t*)pic.data[0];
        const uint8_t* up = (const uint8_t*)pic.data[1];
        const uint8_t* vp = (const uint8_t*)pic.data[2];
        const ptrdiff_t ys = pic.stride[0], cs = pic.stride[1];
        const int ss = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 ? 1 : 0;

        for (int y = 0; y < h; ++y)
        {
            const uint8_t* yrow = yp + (ptrdiff_t)y * ys;
            const uint8_t* urow = up + (ptrdiff_t)(y >> ss) * cs;
            const uint8_t* vrow = vp + (ptrdiff_t)(y >> ss) * cs;
            uint8_t* out = dst + (size_t)y * dstStride;
            for (int x = 0; x < w; ++x)
            {
                const int c = 298 * (yrow[x] - 16);
                const int d = urow[x >> ss] - 128;
                const int e = vrow[x >> ss] - 128;
                out[x * 4]     = Clamp((c + 516 * d + 128) >> 8);           /* B */
                out[x * 4 + 1] = Clamp((c - 100 * d - 208 * e + 128) >> 8); /* G */
                out[x * 4 + 2] = Clamp((c + 409 * e + 128) >> 8);           /* R */
                out[x * 4 + 3] = 0xFF;
            }
        }
    }

    static uint8_t Clamp(int v)
    {
        return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    Dav1dContext* ctx = nullptr;
};
