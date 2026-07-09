/* viewer_x11.cpp - Linux/X11 port of viewer.cpp: connect to a streamserver,
 * receive length-prefixed scap packets, decode them and paint them in an X11
 * window (runs under XWayland on Wayland too). Like scapdec, the codec is
 * selected at RUNTIME by each packet's magic (scap_packet.h): zstd+palette
 * packets blit into an 8bpp canvas, AV1 packets decode through dav1d into a
 * BGRA canvas. --codec only chooses what the server is asked to encode with;
 * the decoder follows whatever arrives.
 * Mouse input over the window (move, left/right down/up, wheel) is forwarded
 * to the server as fixed-size ScapInputMsg records for SendInput emulation.
 *
 * Build: g++ -O2 -I../dav1d/include -I../zstd-1.5.7/lib -o viewer viewer_x11.cpp \
 *            ../zstd-1.5.7/lib/libzstd.a -lX11 -l:libdav1d.so.6 -lpthread
 *        (or just ./connect.sh)
 * Usage: ./viewer [host] [--addr=host[:port]] [--codec=spec]
 *   host/--addr  default 127.0.0.1, port SCAP_STREAM_PORT (a bare token is a
 *                host too, so "./viewer 10.1.110.27" keeps working)
 *   --codec      default config.h SCAP_CODEC_DEFAULT; sent to the server in
 *                the opening ScapHello, which encodes with it ("zstd[:level]",
 *                "av1[:i420|:i444]")
 *        ./viewer --selftest   (headless decode check; no X, no network)
 *
 * Same shape as the Windows viewer: a reader thread does the blocking frame
 * reads + decode (decoder guarded by one mutex) and posts dirty rects to the
 * GUI thread over a self-pipe; the GUI thread owns every Xlib call, so no
 * XInitThreads is needed. scapdec's GDI canvas (DIBSection + BitBlt) is
 * replaced by an inline port of its decode loop onto a plain byte buffer;
 * paint converts dirty pixels to 32-bit and XPutImage's them.
 * ponytail: single connection, no reconnect (restart the viewer, same as
 * Windows). Assumes a little-endian TrueColor 24/32bpp visual - true of any
 * modern x86 desktop; exotic visuals would need XVisualInfo-driven conversion.
 */
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mutex>
#include <thread>
#include <vector>
#include "../common/config.h"
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "../common/scap_stream.h"
#include "../common/av1_decoder.h"
#include "../common/zstd_stream.h"
/* X11 last: X.h #defines None (and other short names), which would break
 * parsing zs::Flush::None in zstd_stream.h if included first. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

struct DirtyRect { int x0, y0, x1, y1; }; /* half-open; empty when x1<=x0 */

/* The server reads exactly this many bytes per input record off the socket;
 * a size mismatch would desync every event after the first. */
static_assert(sizeof(ScapInputMsg) == 16, "ScapInputMsg wire size");

/* Decoder state: scapdec.cpp's ScapDec minus the GDI objects; the canvas
 * format follows the packets (bgra flips per magic). Stride == width (8bpp)
 * / width*4 (BGRA) - no DIB row alignment on a plain buffer. Either decoder
 * persists across packets and needs them in stream order: zstd packets
 * continue one flushed frame, AV1 packets are one temporal unit each but
 * reference earlier frames. */
struct Dec
{
    int width = 0, height = 0;
    bool bgra = false;           /* canvas format: BGRA32 (av1) vs 8bpp (zstd) */
    std::vector<uint8_t> canvas; /* top-down */
    std::vector<uint8_t> raw;    /* zstd decompress scratch */
    zs::StreamDecompressor zdec;
    Av1Dec av1;                  /* call av1.Init() before first packet */
};

static Dec           g_dec;
static std::mutex    g_lock;    /* guards g_dec + g_status (decode vs. paint) */
static SOCKET        g_sock = INVALID_SOCKET;
static volatile bool g_running = true;
static const char*   g_host = "127.0.0.1";
static int           g_port = SCAP_STREAM_PORT;
static char          g_codec[sizeof(ScapHello::codec)] = SCAP_CODEC_DEFAULT;
static int           g_pipe[2]; /* reader -> GUI: DirtyRect per decoded frame */

/* title-bar stats (written by reader thread, read by GUI timer; benign race,
 * same as the Windows viewer) */
static unsigned g_frames, g_bytes, g_lastPktSize;
static char     g_status[64] = "connecting";

/* Port of ScapDec_DecodePacket without the GDI canvas. Returns 0 on success,
 * <0 on malformed packet or decompression failure; *out gets the union of the
 * updated rects. */
static int DecodePacket(Dec* d, const void* packet, unsigned size, DirtyRect* out)
{
    *out = {0, 0, 0, 0};
    if (!packet || size < sizeof(ScapFrameHdr))
        return -1;

    ScapFrameHdr hdr;
    memcpy(&hdr, packet, sizeof(hdr));

    /* AV1 packet: header + one temporal unit; the decoded picture is always
     * a full frame, so the dirty bounds are the whole canvas. */
    if (hdr.magic == SCAP_MAGIC_AV1)
    {
        if (hdr.width == 0 || hdr.height == 0 ||
            hdr.rawSize != size - sizeof(hdr))
            return -1;

        if (hdr.width != d->width || hdr.height != d->height || !d->bgra)
        {
            d->width = hdr.width;
            d->height = hdr.height;
            d->bgra = true;
            d->canvas.assign((size_t)d->width * d->height * 4, 0);
        }

        if (!d->av1.DecodeToBGRA((const uint8_t*)packet + sizeof(hdr),
                                 size - sizeof(hdr), d->canvas.data(),
                                 d->width * 4, d->width, d->height))
            return -3;

        *out = { 0, 0, d->width, d->height };
        return 0;
    }

    if (hdr.magic != SCAP_MAGIC || hdr.rectCount + hdr.moveCount == 0 ||
        hdr.width == 0 || hdr.height == 0)
        return -1;

    if (hdr.width != d->width || hdr.height != d->height || d->bgra)
    {
        d->width = hdr.width;
        d->height = hdr.height;
        d->bgra = false;
        d->canvas.assign((size_t)d->width * d->height, 0); /* RGB332 index 0 = black */
    }

    if (d->raw.size() < hdr.rawSize)
        d->raw.resize(hdr.rawSize);

    /* Decode the blob through the sink into raw, expecting exactly rawSize
     * bytes out (the encoder flushed everything for this packet). Excess
     * output means the packet lies about rawSize: clamp the copy and fail. */
    size_t got = 0;
    bool over = false;
    try
    {
        d->zdec.decompress((const uint8_t*)packet + sizeof(hdr),
                           size - sizeof(hdr),
                           [d, &hdr, &got, &over](const void* p, size_t n) {
                               if (got + n > hdr.rawSize)
                               {
                                   over = true;
                                   n = hdr.rawSize - got;
                               }
                               memcpy(d->raw.data() + got, p, n);
                               got += n;
                           });
    }
    catch (...)
    {
        return -3;
    }
    if (over || got != hdr.rawSize)
        return -3;

    const uint8_t* p = d->raw.data();
    const uint8_t* end = p + hdr.rawSize;
    DirtyRect b = { INT_MAX, INT_MAX, 0, 0 };

    /* Copy ops first (the DXGI/CopyRect contract: moves before dirty), in
     * array order. src and dst may overlap (scroll), so pick the row walk
     * direction like memmove does; within a row memmove handles overlap. */
    for (unsigned i = 0; i < hdr.moveCount; ++i)
    {
        ScapMoveRect mv;
        if (end - p < (ptrdiff_t)sizeof(mv))
            return -4;
        memcpy(&mv, p, sizeof(mv));
        p += sizeof(mv);

        if (mv.w == 0 || mv.h == 0 || mv.x + mv.w > d->width ||
            mv.y + mv.h > d->height || mv.srcX + mv.w > d->width ||
            mv.srcY + mv.h > d->height)
            return -4;

        uint8_t* src = d->canvas.data() + (size_t)mv.srcY * d->width + mv.srcX;
        uint8_t* dst = d->canvas.data() + (size_t)mv.y * d->width + mv.x;
        if (mv.y > mv.srcY) /* downward move: walk rows bottom-up */
        {
            src += (size_t)(mv.h - 1) * d->width;
            dst += (size_t)(mv.h - 1) * d->width;
            for (int row = 0; row < mv.h; ++row, src -= d->width, dst -= d->width)
                memmove(dst, src, mv.w);
        }
        else
        {
            for (int row = 0; row < mv.h; ++row, src += d->width, dst += d->width)
                memmove(dst, src, mv.w);
        }

        if (mv.x < b.x0) b.x0 = mv.x;
        if (mv.y < b.y0) b.y0 = mv.y;
        if (mv.x + mv.w > b.x1) b.x1 = mv.x + mv.w;
        if (mv.y + mv.h > b.y1) b.y1 = mv.y + mv.h;
    }

    for (unsigned i = 0; i < hdr.rectCount; ++i)
    {
        ScapRectHdr rc;
        if (end - p < (ptrdiff_t)sizeof(rc))
            return -4;
        memcpy(&rc, p, sizeof(rc));
        p += sizeof(rc);

        size_t pixels = (size_t)rc.w * rc.h;
        if (rc.w == 0 || rc.h == 0 || rc.x + rc.w > d->width ||
            rc.y + rc.h > d->height || (size_t)(end - p) < pixels)
            return -4;

        uint8_t* dst = d->canvas.data() + (size_t)rc.y * d->width + rc.x;
        for (int row = 0; row < rc.h; ++row, dst += d->width, p += rc.w)
            memcpy(dst, p, rc.w);

        if (rc.x < b.x0) b.x0 = rc.x;
        if (rc.y < b.y0) b.y0 = rc.y;
        if (rc.x + rc.w > b.x1) b.x1 = rc.x + rc.w;
        if (rc.y + rc.h > b.y1) b.y1 = rc.y + rc.h;
    }

    *out = b;
    return 0;
}

static SOCKET ConnectTo(const char* host, int port)
{
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portStr, &hints, &res) != 0)
        return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = res; p; p = p->ai_next)
    {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

static void SetStatus(const char* s)
{
    std::lock_guard<std::mutex> l(g_lock);
    snprintf(g_status, sizeof(g_status), "%s", s);
}

static void ReaderThread(void)
{
    g_sock = ConnectTo(g_host, g_port);
    if (g_sock == INVALID_SOCKET)
    {
        SetStatus("connect failed");
        return;
    }
    /* First bytes on the wire: tell the server which codec to encode with. */
    ScapHello hello = {};
    hello.magic = SCAP_HELLO_MAGIC;
    snprintf(hello.codec, sizeof(hello.codec), "%s", g_codec);
    if (!ScapSendAll(g_sock, &hello, sizeof(hello)))
    {
        SetStatus("hello send failed");
        return;
    }
    SetStatus("streaming");

    std::vector<char> frame;
    while (g_running && ScapRecvFrame(g_sock, frame))
    {
        DirtyRect r;
        int rc;
        {
            std::lock_guard<std::mutex> l(g_lock);
            rc = DecodePacket(&g_dec, frame.data(), (unsigned)frame.size(), &r);
        }
        if (rc == 0)
        {
            ++g_frames;
            g_bytes += (unsigned)frame.size();
            g_lastPktSize = (unsigned)frame.size();
            /* 16-byte write < PIPE_BUF, so atomic; EPIPE means the GUI quit */
            if (r.x1 > r.x0 && write(g_pipe[1], &r, sizeof(r)) != sizeof(r))
                break;
        }
    }
    if (g_running)
        SetStatus("disconnected");
}

/* Port of viewer.cpp's SendMouse: map a client-area point to a normalized
 * 0..65535 desktop position (decoded canvas size = server desktop size) and
 * send it as one input message. Runs on the GUI thread, the only sender, so
 * sends are serialized; the reader thread only receives, so send/recv share
 * g_sock safely. No-op until connected and a frame has arrived. */
static void SendMouse(uint16_t type, int cx, int cy, int wheel)
{
    if (g_sock == INVALID_SOCKET)
        return;
    int w, h;
    {
        std::lock_guard<std::mutex> l(g_lock);
        w = g_dec.width;
        h = g_dec.height;
    }
    if (w <= 1 || h <= 1)
        return; /* no canvas yet */
    if (cx < 0) cx = 0; else if (cx > w - 1) cx = w - 1;
    if (cy < 0) cy = 0; else if (cy > h - 1) cy = h - 1;
    ScapInputMsg m = {};
    m.type = type;
    m.x = (uint32_t)((int64_t)cx * 65535 / (w - 1));
    m.y = (uint32_t)((int64_t)cy * 65535 / (h - 1));
    m.wheel = wheel;
    ScapSendAll(g_sock, &m, sizeof(m));
}

/* Convert the canvas region [x0,y0)-[x1,y1) to 32bpp and put it on the window
 * at the same position. Clamps to the canvas; caller passes window coords. */
static void PaintRect(Display* dpy, Window win, GC gc,
                      int x0, int y0, int x1, int y1)
{
    std::lock_guard<std::mutex> l(g_lock);
    if (g_dec.width == 0)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_dec.width)  x1 = g_dec.width;
    if (y1 > g_dec.height) y1 = g_dec.height;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;

    uint32_t* buf = (uint32_t*)malloc((size_t)w * h * 4);
    if (!buf)
        return;
    if (g_dec.bgra)
    {
        /* canvas is already BGRA bytes = 0xAARRGGBB little-endian words,
         * exactly what a ZPixmap on a LE TrueColor visual wants: straight
         * row copies. */
        for (int row = 0; row < h; ++row)
            memcpy(buf + (size_t)row * w,
                   g_dec.canvas.data() +
                       ((size_t)(y0 + row) * g_dec.width + x0) * 4,
                   (size_t)w * 4);
    }
    else
    {
        for (int row = 0; row < h; ++row)
        {
            const uint8_t* src = g_dec.canvas.data() +
                                 (size_t)(y0 + row) * g_dec.width + x0;
            uint32_t* dst = buf + (size_t)row * w;
            for (int col = 0; col < w; ++col)
            {
                ScapPalEntry e = kScapPal[src[col]];
                dst[col] = ((uint32_t)e.r << 16) | ((uint32_t)e.g << 8) | e.b;
            }
        }
    }

    int scr = DefaultScreen(dpy);
    XImage* img = XCreateImage(dpy, DefaultVisual(dpy, scr),
                               DefaultDepth(dpy, scr), ZPixmap, 0,
                               (char*)buf, w, h, 32, w * 4);
    if (!img)
    {
        free(buf);
        return;
    }
    XPutImage(dpy, win, gc, img, 0, 0, x0, y0, w, h);
    XDestroyImage(img); /* frees buf */
}

static uint64_t NowMs(void)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Headless check of both decode paths and the per-packet codec dispatch.
 *
 * AV1: a pre-encoded 64x64 solid BGRA(200,100,50) keyframe (produced by
 * Av1Enc itself, regenerate with the av1_blobgen scratch tool if the wire
 * format ever changes) must decode through dav1d into the expected color.
 * Roundtrip error of the embedded frame is <=1 per channel; tolerance 8
 * leaves headroom for future encoder retuning.
 *
 * zstd: packets flushed from one zs::StreamCompressor (later packets continue
 * the zstd frame, so decoding them proves the DCtx persists across packets),
 * fed into the SAME Dec right after AV1 - the first one must flip the canvas
 * back to 8bpp. Covers pixel rects, an overlapping downward move rect (fails
 * if the row walk direction is wrong), bounds, and malformed-packet
 * rejection. */
static const unsigned char kAv1TestKeyframe[] = {
    18,0,10,10,0,0,0,2,175,255,154,95,32,8,50,25,
    20,0,37,0,0,0,132,1,0,2,194,23,111,41,2,38,
    185,12,170,254,227,104,141,76,184
};

static int SelfTest(void)
{
    Dec d;
    assert(d.av1.Init());
    DirtyRect b;

    /* --- AV1 path --- */
    {
        ScapFrameHdr hdr = { SCAP_MAGIC_AV1, 64, 64, 0, 0,
                             (uint32_t)sizeof(kAv1TestKeyframe) };
        std::vector<uint8_t> pkt(sizeof(hdr) + sizeof(kAv1TestKeyframe));
        memcpy(pkt.data(), &hdr, sizeof(hdr));
        memcpy(pkt.data() + sizeof(hdr), kAv1TestKeyframe,
               sizeof(kAv1TestKeyframe));

        assert(DecodePacket(&d, pkt.data(), (unsigned)pkt.size(), &b) == 0);
        assert(d.bgra && d.width == 64 && d.height == 64);
        assert(b.x0 == 0 && b.y0 == 0 && b.x1 == 64 && b.y1 == 64);
        /* solid BGRA(200,100,50): check a corner and the center */
        for (size_t at : { (size_t)0, (size_t)(32 * 64 + 32) * 4 })
        {
            assert(abs(d.canvas[at] - 200) <= 8);     /* B */
            assert(abs(d.canvas[at + 1] - 100) <= 8); /* G */
            assert(abs(d.canvas[at + 2] - 50) <= 8);  /* R */
        }
        /* unknown magic must be rejected, not misparsed */
        const uint32_t bad = 0xDEADBEEFu;
        memcpy(pkt.data(), &bad, sizeof(bad));
        assert(DecodePacket(&d, pkt.data(), (unsigned)pkt.size(), &b) != 0);
        /* truncated packet */
        memcpy(pkt.data(), &hdr, sizeof(hdr));
        assert(DecodePacket(&d, pkt.data(), sizeof(hdr) - 1, &b) != 0);
    }

    /* --- zstd path, into the same Dec: runtime dispatch flips the canvas --- */
    zs::StreamCompressor enc;
    auto build = [&enc](const ScapFrameHdr& hdr,
                        const std::vector<uint8_t>& payload,
                        std::vector<uint8_t>& pkt) {
        pkt.resize(sizeof(hdr));
        enc.compressFrame(payload.data(), payload.size(),
                          [&pkt](const void* p, size_t n) {
                              pkt.insert(pkt.end(), (const uint8_t*)p,
                                         (const uint8_t*)p + n);
                          });
        memcpy(pkt.data(), &hdr, sizeof(hdr));
    };

    /* pkt1: two pixel rects on an 8x5 canvas */
    std::vector<uint8_t> payload;
    const ScapRectHdr r1 = { 1, 1, 3, 2 }, r2 = { 0, 0, 2, 1 };
    payload.insert(payload.end(), (const uint8_t*)&r1, (const uint8_t*)(&r1 + 1));
    payload.insert(payload.end(), (size_t)r1.w * r1.h, 7);
    payload.insert(payload.end(), (const uint8_t*)&r2, (const uint8_t*)(&r2 + 1));
    payload.insert(payload.end(), (size_t)r2.w * r2.h, 3);
    ScapFrameHdr hdr1 = { SCAP_MAGIC, 8, 5, 2, 0, (uint32_t)payload.size() };
    std::vector<uint8_t> pkt;
    build(hdr1, payload, pkt);
    assert(DecodePacket(&d, pkt.data(), (unsigned)pkt.size(), &b) == 0);
    assert(!d.bgra && d.width == 8 && d.height == 5);
    assert(d.canvas[1 * 8 + 1] == 7 && d.canvas[2 * 8 + 3] == 7); /* r1 */
    assert(d.canvas[0] == 3 && d.canvas[1] == 3);                 /* r2 */
    assert(d.canvas[4 * 8 + 7] == 0);              /* untouched = black (RGB332) */
    assert(b.x0 == 0 && b.y0 == 0 && b.x1 == 4 && b.y1 == 3);

    /* pkt2: mid-frame continuation - decodable only with the same DCtx, in
     * order; overwrites row 1 of r1's block with 9s so the rows differ */
    payload.clear();
    const ScapRectHdr r3 = { 1, 1, 3, 1 };
    payload.insert(payload.end(), (const uint8_t*)&r3, (const uint8_t*)(&r3 + 1));
    payload.insert(payload.end(), (size_t)r3.w * r3.h, 9);
    ScapFrameHdr hdr2 = { SCAP_MAGIC, 8, 5, 1, 0, (uint32_t)payload.size() };
    build(hdr2, payload, pkt);
    assert(DecodePacket(&d, pkt.data(), (unsigned)pkt.size(), &b) == 0);
    assert(d.canvas[1 * 8 + 1] == 9 && d.canvas[2 * 8 + 1] == 7);

    /* pkt3: overlapping downward move (rows 1-2 -> 2-3). A correct bottom-up
     * walk yields row2=9,row3=7; a top-down walk would smear 9 into row 3. */
    payload.clear();
    const ScapMoveRect mv = { 1, 1, 1, 2, 3, 2 };
    payload.insert(payload.end(), (const uint8_t*)&mv, (const uint8_t*)(&mv + 1));
    ScapFrameHdr hdr3 = { SCAP_MAGIC, 8, 5, 0, 1, (uint32_t)payload.size() };
    build(hdr3, payload, pkt);
    assert(DecodePacket(&d, pkt.data(), (unsigned)pkt.size(), &b) == 0);
    assert(d.canvas[2 * 8 + 1] == 9 && d.canvas[3 * 8 + 1] == 7);
    assert(b.x0 == 1 && b.y0 == 2 && b.x1 == 4 && b.y1 == 4);

    printf("selftest ok (zstd+av1 runtime dispatch)\n");
    return 0;
}

int main(int argc, char** argv)
{
    /* --addr=host[:port] --codec=spec, same flags as viewer.cpp; a bare
     * token is a host (keeps "./viewer 10.1.110.27" / connect.sh working). */
    for (int i = 1; i < argc; ++i)
    {
        char* t = argv[i];
        if (strcmp(t, "--selftest") == 0)
            return SelfTest();
        if (strncmp(t, "--addr=", 7) == 0 && t[7])
        {
            char* hp = t + 7;
            if (char* colon = strrchr(hp, ':'))
            {
                *colon = '\0';
                int port = atoi(colon + 1);
                if (port > 0 && port < 65536)
                    g_port = port;
            }
            if (*hp)
                g_host = hp;
        }
        else if (strncmp(t, "--codec=", 8) == 0 && t[8])
            snprintf(g_codec, sizeof(g_codec), "%s", t + 8);
        else if (t[0] != '-')
            g_host = t;
    }

    signal(SIGPIPE, SIG_IGN); /* pipe/socket errors come back as EPIPE */
    if (!g_dec.av1.Init())
    {
        fprintf(stderr, "dav1d_open failed\n");
        return 1;
    }
    if (pipe(g_pipe) != 0)
    {
        perror("pipe");
        return 1;
    }
    fcntl(g_pipe[0], F_SETFL, O_NONBLOCK); /* GUI drains without blocking */

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy)
    {
        fprintf(stderr, "cannot open X display\n");
        return 1;
    }
    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, 1280, 800,
                                     0, BlackPixel(dpy, scr),
                                     0x404040 /* DKGRAY, TrueColor assumed */);
    XStoreName(dpy, win, "scap viewer");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                               PointerMotionMask);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XMapWindow(dpy, win);
    GC gc = DefaultGC(dpy, scr);
    XFlush(dpy);

    std::thread reader(ReaderThread);
    uint64_t fpsTick = NowMs();

    /* ~3s sliding window for avg packet size / data rate, built from the last
     * few title intervals (each >=1s). ponytail: window is quantized to title
     * ticks, so it spans 3..4.5s rather than exactly 3s; per-packet timestamps
     * would be needed for an exact window. */
    struct { unsigned bytes, pkts; uint64_t ms; } statWin[4] = {};
    int wi = 0;

    pollfd fds[2] = { { ConnectionNumber(dpy), POLLIN, 0 },
                      { g_pipe[0], POLLIN, 0 } };
    bool quit = false;
    int lastW = 0, lastH = 0; /* canvas size the window was last sized to */
    while (!quit)
    {
        poll(fds, 2, 500);

        while (XPending(dpy))
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose)
                PaintRect(dpy, win, gc, ev.xexpose.x, ev.xexpose.y,
                          ev.xexpose.x + ev.xexpose.width,
                          ev.xexpose.y + ev.xexpose.height);
            else if (ev.type == MotionNotify)
                SendMouse(SCAP_IN_MOVE, ev.xmotion.x, ev.xmotion.y, 0);
            else if (ev.type == ButtonPress || ev.type == ButtonRelease)
            {
                /* X wheel = buttons 4/5, one press+release per notch: send
                 * WHEEL_DELTA units on press only (matches WM_MOUSEWHEEL).
                 * Drags that leave the window keep reporting via X's implicit
                 * press-to-release grab - the SetCapture equivalent. */
                bool down = (ev.type == ButtonPress);
                int x = ev.xbutton.x, y = ev.xbutton.y;
                switch (ev.xbutton.button)
                {
                case Button1:
                    SendMouse(down ? SCAP_IN_LDOWN : SCAP_IN_LUP, x, y, 0);
                    break;
                case Button3:
                    SendMouse(down ? SCAP_IN_RDOWN : SCAP_IN_RUP, x, y, 0);
                    break;
                case Button4:
                    if (down) SendMouse(SCAP_IN_WHEEL, x, y, 120);
                    break;
                case Button5:
                    if (down) SendMouse(SCAP_IN_WHEEL, x, y, -120);
                    break;
                }
            }
            else if (ev.type == ClientMessage &&
                     (Atom)ev.xclient.data.l[0] == wmDelete)
                quit = true;
        }

        /* Drain all pending dirty rects into one union -> one repaint. */
        DirtyRect u = { INT_MAX, INT_MAX, 0, 0 }, r;
        while (read(g_pipe[0], &r, sizeof(r)) == (ssize_t)sizeof(r))
        {
            if (r.x0 < u.x0) u.x0 = r.x0;
            if (r.y0 < u.y0) u.y0 = r.y0;
            if (r.x1 > u.x1) u.x1 = r.x1;
            if (r.y1 > u.y1) u.y1 = r.y1;
        }
        /* First frame (or server resolution change): snap the window to the
         * canvas so the 1:1 blit fits exactly. Clamped to the screen size;
         * ponytail: no scaling, a canvas larger than the screen is cropped -
         * scaling the blit in PaintRect would be the upgrade path. */
        int cw, ch;
        {
            std::lock_guard<std::mutex> l(g_lock);
            cw = g_dec.width;
            ch = g_dec.height;
        }
        if (cw && (cw != lastW || ch != lastH))
        {
            int w = cw < DisplayWidth(dpy, scr) ? cw : DisplayWidth(dpy, scr);
            int h = ch < DisplayHeight(dpy, scr) ? ch : DisplayHeight(dpy, scr);
            XResizeWindow(dpy, win, w, h);
            lastW = cw;
            lastH = ch;
        }

        if (u.x1 > u.x0)
            PaintRect(dpy, win, gc, u.x0, u.y0, u.x1, u.y1);

        uint64_t now = NowMs();
        uint64_t elapsed = now - fpsTick;
        if (elapsed >= 1000)
        {
            char status[sizeof(g_status)], buf[160];
            {
                std::lock_guard<std::mutex> l(g_lock);
                memcpy(status, g_status, sizeof(status));
            }
            unsigned frames = g_frames, bytes = g_bytes;
            g_frames = 0;
            g_bytes = 0;

            statWin[wi] = { bytes, frames, elapsed };
            wi = (wi + 1) % 4;
            uint64_t wb = 0, wp = 0, wms = 0;
            for (int i = 0; i < 4 && wms < 3000; ++i)
            {
                int k = (wi + 3 - i) % 4; /* newest first */
                wb += statWin[k].bytes;
                wp += statWin[k].pkts;
                wms += statWin[k].ms;
            }

            snprintf(buf, sizeof(buf),
                     "scap viewer [%s:%d] - %s, %u fps, last pkt %u B, "
                     "avg %u B/pkt %.1f KB/s (3s), %s",
                     g_host, g_port, g_codec,
                     (unsigned)(frames * 1000 / elapsed), g_lastPktSize,
                     (unsigned)(wp ? wb / wp : 0),
                     wms ? (double)wb * 1000.0 / (double)wms / 1024.0 : 0.0,
                     status);
            XStoreName(dpy, win, buf);
            fpsTick = now;
        }
        XFlush(dpy);
    }

    /* Tear down the reader: stop the loop, unblock a pending recv, and close
     * the pipe's read end so a reader blocked in write() gets EPIPE. */
    g_running = false;
    if (g_sock != INVALID_SOCKET)
        shutdown(g_sock, SD_BOTH);
    close(g_pipe[0]);
    reader.join();
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);
    close(g_pipe[1]);

    XCloseDisplay(dpy);
    return 0;
}
