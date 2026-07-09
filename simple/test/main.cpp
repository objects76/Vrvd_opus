/* main.cpp - loopback test: capture the primary desktop via scapenc, feed the
 * packets to scapdec, paint the decoded 256-color canvas in a window.
 * Title bar shows fps / packet size / capture status.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "scapenc.h"
#include "scapdec.h"
#include "config.h"
#include "scap_packet.h"
#include "scap_palette.h"
#include "zstd.h"
#include "zstd_stream.h"

static ScapEnc* g_enc;
static ScapDec* g_dec;
static unsigned g_frames, g_lastPktSize;
static DWORD    g_fpsTick;
static char     g_status[32] = "run";

static void UpdateTitle(HWND hwnd)
{
    DWORD now = GetTickCount();
    if (now - g_fpsTick < 1000)
        return;
    char buf[128];
    _snprintf_s(buf, _TRUNCATE, "scap loopback - %u fps, last pkt %u B, %s",
                g_frames * 1000 / (now - g_fpsTick), g_lastPktSize, g_status);
    SetWindowTextA(hwnd, buf);
    g_frames = 0;
    g_fpsTick = now;
}

static void Pump(HWND hwnd)
{
    /* Drain everything currently available (timeout 0). */
    for (;;)
    {
        const void* pkt;
        unsigned n;
        int rc = ScapEnc_CaptureFrame(g_enc, 0, &pkt, &n);
        if (rc == SCAPENC_OK)
        {
            strcpy_s(g_status, "run");
            g_lastPktSize = n;
            ++g_frames;
            RECT bounds;
            if (ScapDec_DecodePacket(g_dec, pkt, n, &bounds) == 0)
                InvalidateRect(hwnd, &bounds, FALSE);
            continue;
        }
        if (rc == SCAPENC_AGAIN)
            strcpy_s(g_status, "again (dup lost)");
        else if (rc == SCAPENC_ERR)
            strcpy_s(g_status, "error");
        break; /* TIMEOUT / NOCHANGE / AGAIN / ERR: try again on next timer */
    }
    UpdateTitle(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        Pump(hwnd);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (ScapDec_Paint(g_dec, hdc, 0, 0, &ps.rcPaint) != 0)
            FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    /* The class is registered with RegisterClassA, so the ANSI DefWindowProc
     * must be used; DefWindowProcW would reinterpret ANSI WM_SETTEXT strings
     * as wide chars (title bar showed CJK mojibake). */
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- packet test (`test.exe -packettest`) -------------------------------
 * Decoder-only unit test for the wire format, especially move (copy) ops:
 * builds packets by hand, decodes them, and compares the painted canvas
 * against a trivially-correct reference model. No DXGI needed. */

struct TestRect { ScapRectHdr h; const uint8_t* px; };

/* Without zc the blob is a standalone zstd frame (one-shot ZSTD_compress);
 * with zc it is a flushed piece of zc's continuing frame, exactly what the
 * real encoder produces - both must decode through the same decoder. */
static std::vector<uint8_t> BuildPacket(int w, int h,
                                        const std::vector<ScapMoveRect>& moves,
                                        const std::vector<TestRect>& rects,
                                        zs::StreamCompressor* zc = nullptr)
{
    std::vector<uint8_t> raw;
    for (const ScapMoveRect& m : moves)
        raw.insert(raw.end(), (const uint8_t*)&m, (const uint8_t*)(&m + 1));
    for (const TestRect& r : rects)
    {
        raw.insert(raw.end(), (const uint8_t*)&r.h, (const uint8_t*)(&r.h + 1));
        raw.insert(raw.end(), r.px, r.px + (size_t)r.h.w * r.h.h);
    }

    std::vector<uint8_t> pkt;
    if (zc)
    {
        pkt.resize(sizeof(ScapFrameHdr));
        zc->compressFrame(raw.data(), raw.size(),
                          [&pkt](const void* p, size_t n) {
                              pkt.insert(pkt.end(), (const uint8_t*)p,
                                         (const uint8_t*)p + n);
                          });
    }
    else
    {
        pkt.resize(sizeof(ScapFrameHdr) + ZSTD_compressBound(raw.size()));
        size_t zLen = ZSTD_compress(pkt.data() + sizeof(ScapFrameHdr),
                                    pkt.size() - sizeof(ScapFrameHdr),
                                    raw.data(), raw.size(),
                                    ZSTD_CLEVEL_DEFAULT);
        pkt.resize(sizeof(ScapFrameHdr) + zLen);
    }

    ScapFrameHdr hdr = {};
    hdr.magic = SCAP_MAGIC;
    hdr.width = (uint16_t)w;
    hdr.height = (uint16_t)h;
    hdr.rectCount = (uint16_t)rects.size();
    hdr.moveCount = (uint16_t)moves.size();
    hdr.rawSize = (uint32_t)raw.size();
    memcpy(pkt.data(), &hdr, sizeof(hdr));
    return pkt;
}

/* Reference model: same ops on a plain w*h index buffer. Moves copy through
 * a temp buffer, so overlap is correct by construction. */
static void RefMove(std::vector<uint8_t>& ref, int w, const ScapMoveRect& m)
{
    std::vector<uint8_t> tmp((size_t)m.w * m.h);
    for (int row = 0; row < m.h; ++row)
        memcpy(&tmp[(size_t)row * m.w],
               &ref[(size_t)(m.srcY + row) * w + m.srcX], m.w);
    for (int row = 0; row < m.h; ++row)
        memcpy(&ref[(size_t)(m.y + row) * w + m.x],
               &tmp[(size_t)row * m.w], m.w);
}

static void RefRect(std::vector<uint8_t>& ref, int w, const TestRect& r)
{
    for (int row = 0; row < r.h.h; ++row)
        memcpy(&ref[(size_t)(r.h.y + row) * w + r.h.x],
               r.px + (size_t)row * r.h.w, r.h.w);
}

/* Paint the decoder canvas into a 32bpp DIB and compare each pixel with the
 * palette-expanded reference (index-level compare via GDI is not reliable:
 * duplicate palette colors may remap). Returns 0 on match. */
static int CompareCanvas(ScapDec* dec, const std::vector<uint8_t>& ref,
                         int w, int h)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (!dib)
        return 1;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, dib);

    RECT full = { 0, 0, w, h };
    int bad = ScapDec_Paint(dec, dc, 0, 0, &full) != 0;
    GdiFlush();
    for (int i = 0; i < w * h && !bad; ++i)
    {
        const uint8_t* px = (const uint8_t*)bits + (size_t)i * 4; /* BGRA */
        const ScapPalEntry& pe = kScapPal[ref[i]];
        bad = px[0] != pe.b || px[1] != pe.g || px[2] != pe.r;
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(dib);
    return bad;
}

static int PacketTest(void)
{
    /* Hand-built zstd/palette wire-format packets; scapdec dispatches on the
     * packet magic, so this runs regardless of what codec an encoder would
     * use. The AV1 pipeline is covered by -selftest. */
#ifndef SCAP_USE_256MAP
    /* 0: palette/quantizer roundtrip - expanding an index through kScapPal
     * and re-quantizing at (0,0) (Bayer offset 0) must return the same
     * index; fails if the RGB332 pack/expand shifts ever disagree. Not
     * meaningful for the 256map backend: its palette has duplicate colors
     * and the LUT truncates to RGB555 first. */
    for (int i = 0; i < 256; ++i)
    {
        uint8_t px[4] = { kScapPal[i].b, kScapPal[i].g, kScapPal[i].r, 0 };
        if (ScapQuant332(px, 0, 0) != i)
            return 10;
    }
#endif

    /* 0b: rect quantizer (AVX2 path where the CPU has it) must match the
     * per-pixel adapter byte-for-byte, with dither off AND on. Odd width
     * exercises the vector tail, odd x0/y0 the Bayer phase alignment,
     * stride > w*4 the row walk. */
    for (int dither = 0; dither <= 1; ++dither)
    {
        const int TW = 61, TH = 13, STRIDE = (TW + 3) * 4;
        std::vector<uint8_t> img((size_t)STRIDE * TH);
        for (size_t i = 0; i < img.size(); ++i)
            img[i] = (uint8_t)((i * 2654435761u) >> 13); /* incl. near-255 */
        ScapQuant q;
        ScapQuantInit(&q, dither);
        std::vector<uint8_t> got((size_t)TW * TH);
        ScapQuantRect(&q, img.data(), STRIDE, got.data(), TW, TH, 3, 2);
        for (int yy = 0; yy < TH; ++yy)
            for (int xx = 0; xx < TW; ++xx)
                if (got[(size_t)yy * TW + xx] !=
                    ScapQuantPixel(&q, &img[(size_t)yy * STRIDE + xx * 4],
                                   3 + xx, 2 + yy))
                    return 10;
    }

    /* 0c: the dither flag must actually change index selection - sweeping a
     * gray gradient across the 4 Bayer rows has to quantize differently with
     * dithering on (fails if the flag is stored but ignored). */
    {
        ScapQuant qOff, qOn;
        ScapQuantInit(&qOff, 0);
        ScapQuantInit(&qOn, 1);
        bool differs = false;
        for (int v = 0; v < 256 && !differs; ++v)
        {
            uint8_t px[4] = { (uint8_t)v, (uint8_t)v, (uint8_t)v, 0 };
            for (int y = 0; y < 4 && !differs; ++y)
                differs = ScapQuantPixel(&qOff, px, v, y) !=
                          ScapQuantPixel(&qOn, px, v, y);
        }
        if (!differs)
            return 10;
    }

    const int W = 64, H = 48;
    ScapDec* dec = ScapDec_Create();
    std::vector<uint8_t> ref((size_t)W * H);
    int step = 10;

    /* 1: full-frame pixel rect establishes the canvas (also the
     * moveCount==0 legacy-format regression check). */
    std::vector<uint8_t> pix((size_t)W * H);
    for (int i = 0; i < W * H; ++i)
        pix[i] = (uint8_t)(i * 7 + i / W);
    TestRect fullRect = { { 0, 0, W, H }, pix.data() };
    RECT bounds;
    std::vector<uint8_t> pkt = BuildPacket(W, H, {}, { fullRect });
    if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(), &bounds))
        goto fail;
    RefRect(ref, W, fullRect);
    if (CompareCanvas(dec, ref, W, H))
        goto fail;

    /* 2: pure-move packet (rectCount 0) - upward overlapping scroll,
     * exercises the top-down row walk. Bounds must be the move dest. */
    ++step;
    {
        ScapMoveRect m = { 8, 13, 8, 8, 40, 24 }; /* dst.y < src.y */
        pkt = BuildPacket(W, H, { m }, {});
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(), &bounds))
            goto fail;
        RefMove(ref, W, m);
        RECT want = { 8, 8, 48, 32 };
        if (CompareCanvas(dec, ref, W, H) || !EqualRect(&bounds, &want))
            goto fail;
    }

    /* 3: downward overlapping move (bottom-up row walk) + a dirty rect in
     * the same packet; the rect must land after the move. */
    ++step;
    {
        ScapMoveRect m = { 4, 4, 4, 10, 30, 20 }; /* dst.y > src.y */
        std::vector<uint8_t> dpix((size_t)8 * 8, 200);
        TestRect dr = { { 10, 12, 8, 8 }, dpix.data() };
        pkt = BuildPacket(W, H, { m }, { dr });
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(), &bounds))
            goto fail;
        RefMove(ref, W, m);
        RefRect(ref, W, dr);
        if (CompareCanvas(dec, ref, W, H))
            goto fail;
    }

    /* 4: chained moves in one packet - the second sources the first's
     * destination, so array order must be preserved. */
    ++step;
    {
        ScapMoveRect a = { 0, 0, 32, 0, 16, 16 };
        ScapMoveRect b = { 32, 0, 32, 16, 16, 16 };
        pkt = BuildPacket(W, H, { a, b }, {});
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(), &bounds))
            goto fail;
        RefMove(ref, W, a);
        RefMove(ref, W, b);
        if (CompareCanvas(dec, ref, W, H))
            goto fail;
    }

    /* 5: out-of-bounds move must be rejected (decode error, not a crash). */
    ++step;
    {
        ScapMoveRect m = { 40, 0, 50, 0, 20, 8 }; /* dst 50+20 > 64 */
        pkt = BuildPacket(W, H, { m }, {});
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(),
                                 &bounds) != -4)
            goto fail;
        ScapMoveRect m2 = { 50, 0, 40, 0, 20, 8 }; /* src 50+20 > 64 */
        pkt = BuildPacket(W, H, { m2 }, {});
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(),
                                 &bounds) != -4)
            goto fail;
    }

    /* 6: truncated payload - header promises two moves, payload holds one. */
    ++step;
    {
        ScapMoveRect m = { 0, 0, 16, 0, 8, 8 };
        pkt = BuildPacket(W, H, { m }, {});
        ((ScapFrameHdr*)pkt.data())->moveCount = 2;
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(),
                                 &bounds) != -4)
            goto fail;
    }

    /* 7: streaming continuation - two packets flushed from one
     * zs::StreamCompressor (the exact helper scapenc uses) form one
     * continuing zstd frame; the second decodes only if the decoder kept its
     * context and window across packets. Packet A is a full-frame rect:
     * rejected packets (cases 5/6) legally leave partially applied moves on
     * the canvas but not in ref, so the full rewrite re-syncs the two before
     * the final compare. */
    ++step;
    {
        zs::StreamCompressor zc;
        std::vector<uint8_t> p1((size_t)W * H, 55), p2((size_t)16 * 16, 99);
        TestRect t1 = { { 0, 0, W, H }, p1.data() };
        TestRect t2 = { { 16, 16, 16, 16 }, p2.data() };
        pkt = BuildPacket(W, H, {}, { t1 }, &zc);
        std::vector<uint8_t> pktB = BuildPacket(W, H, {}, { t2 }, &zc);
        if (ScapDec_DecodePacket(dec, pkt.data(), (unsigned)pkt.size(), &bounds))
            goto fail;
        RefRect(ref, W, t1);
        if (ScapDec_DecodePacket(dec, pktB.data(), (unsigned)pktB.size(),
                                 &bounds))
            goto fail;
        RefRect(ref, W, t2);
        if (CompareCanvas(dec, ref, W, H))
            goto fail;
    }

    ScapDec_Destroy(dec);
    return 0;
fail:
    ScapDec_Destroy(dec);
    return step; /* 10 + failed case number */
}

/* Capture one frame with the given codec spec, decode it, verify
 * sizes/bounds agree. Returns 0 on pass (same exit codes as before:
 * 1 create, 2 no frame, 3 decode/consistency, 4 capture error). */
static int RoundTrip(const char* codec)
{
    ScapEnc* enc = ScapEnc_Create(codec);
    if (!enc)
        return 1;
    ScapDec* dec = ScapDec_Create();

    int ret = 2; /* no frame within the deadline */
    for (DWORD start = GetTickCount(); GetTickCount() - start < 5000;)
    {
        const void* pkt;
        unsigned n;
        int rc = ScapEnc_CaptureFrame(enc, 100, &pkt, &n);
        if (rc == SCAPENC_OK)
        {
            RECT bounds;
            int ew, eh, dw, dh;
            ret = 3; /* decode/consistency failure */
            if (ScapDec_DecodePacket(dec, pkt, n, &bounds) == 0 &&
                ScapEnc_GetDesktopSize(enc, &ew, &eh) == 0 &&
                ScapDec_GetSize(dec, &dw, &dh) == 0 &&
                ew == dw && eh == dh && !IsRectEmpty(&bounds))
                ret = 0;
            break;
        }
        if (rc == SCAPENC_ERR)
        {
            ret = 4;
            break;
        }
        /* TIMEOUT / NOCHANGE / AGAIN: keep polling */
    }

    ScapDec_Destroy(dec);
    ScapEnc_Destroy(enc);
    return ret;
}

/* Non-interactive check (`test.exe -selftest`): the capture->encode->decode
 * roundtrip must pass for every runtime codec spec. Exit code 0 = pass. */
static int SelfTest(void)
{
    static const char* specs[] = { "zstd:6", "zstd:6:dither", "av1:i420",
                                   "av1:i444" };
    for (const char* spec : specs)
        if (int ret = RoundTrip(spec))
            return ret;
    /* a bad spec must be rejected, not silently defaulted */
    if (ScapEnc_Create("mp4") != nullptr ||
        ScapEnc_Create("zstd:99") != nullptr ||
        ScapEnc_Create("zstd:6:blur") != nullptr)
        return 5;
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
    if (strstr(lpCmdLine, "-packettest"))
        return PacketTest();
    if (strstr(lpCmdLine, "-selftest"))
        return SelfTest();

    /* A bare (non-flag) command line is a codec spec, e.g. `test.exe av1:i444`;
     * empty = config.h SCAP_CODEC_DEFAULT. */
    const char* codec =
        (lpCmdLine && lpCmdLine[0] && lpCmdLine[0] != '-') ? lpCmdLine : nullptr;
    g_enc = ScapEnc_Create(codec);
    if (!g_enc)
    {
        MessageBoxA(nullptr,
                    "ScapEnc_Create failed (bad codec spec / no D3D11 device?)",
                    "scap loopback", MB_ICONERROR);
        return 1;
    }
    g_dec = ScapDec_Create();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "ScapLoopback";
    RegisterClassA(&wc);

    /* Window sized to the desktop (capped), client shows the top-left crop.
     * Topmost so the viewer stays visible over the mirrored content. */
    int w = 1280, h = 800;
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName,
                                "scap loopback", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                                nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    g_fpsTick = GetTickCount();
    SetTimer(hwnd, 1, 15, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ScapDec_Destroy(g_dec);
    ScapEnc_Destroy(g_enc);
    return 0;
}
