// av1_check.cpp - self-check: decode an AV1 IVF stream with dav1d and assert.
// Verifies the viewer's AV1 setup (vendored headers + system libdav1d.so.6).
//
// Generate input (any AV1-capable ffmpeg):
//   ffmpeg -y -f lavfi -i testsrc2=size=320x240:rate=10:duration=0.8 -pix_fmt yuv420p -c:v libaom-av1 -usage realtime -cpu-used 8 -crf 40 -f ivf /tmp/av1_check.ivf
// Build & run:
//   g++ -O2 -Wall -I../dav1d/include -o /tmp/av1_check av1_check.cpp -l:libdav1d.so.6
//   /tmp/av1_check /tmp/av1_check.ivf

#include <dav1d/dav1d.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "/tmp/av1_check.ivf";
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }

    // IVF: 32-byte header, then per frame [4B size LE][8B pts][payload]
    uint8_t hdr[32];
    assert(fread(hdr, 1, 32, f) == 32);
    assert(!memcmp(hdr, "DKIF", 4) && !memcmp(hdr + 8, "AV01", 4));
    const int w = hdr[12] | hdr[13] << 8, h = hdr[14] | hdr[15] << 8;
    const uint32_t nframes = rd32(hdr + 24);
    printf("ivf: %dx%d, %u frames, dav1d %s\n", w, h, nframes, dav1d_version());

    Dav1dSettings s;
    dav1d_default_settings(&s);
    s.max_frame_delay = 1; // low latency, same as WebRTC/CRD decoder
    Dav1dContext* ctx = nullptr;
    assert(dav1d_open(&ctx, &s) == 0);

    uint32_t decoded = 0;
    uint8_t fh[12];
    while (fread(fh, 1, 12, f) == 12) {
        const uint32_t sz = rd32(fh);
        Dav1dData data;
        uint8_t* buf = dav1d_data_create(&data, sz);
        assert(buf && fread(buf, 1, sz, f) == sz);
        do {
            int res = dav1d_send_data(ctx, &data);
            assert(res == 0 || res == DAV1D_ERR(EAGAIN));
            Dav1dPicture pic;
            memset(&pic, 0, sizeof(pic));
            res = dav1d_get_picture(ctx, &pic);
            if (res == 0) {
                assert(pic.p.w == w && pic.p.h == h);
                decoded++;
                dav1d_picture_unref(&pic);
            } else {
                assert(res == DAV1D_ERR(EAGAIN));
            }
        } while (data.sz > 0);
    }
    // drain
    for (;;) {
        Dav1dPicture pic;
        memset(&pic, 0, sizeof(pic));
        if (dav1d_get_picture(ctx, &pic) != 0) break;
        assert(pic.p.w == w && pic.p.h == h);
        decoded++;
        dav1d_picture_unref(&pic);
    }
    dav1d_close(&ctx);
    fclose(f);

    printf("decoded %u/%u frames\n", decoded, nframes);
    assert(decoded == nframes && decoded > 0);
    printf("av1_check OK\n");
    return 0;
}
