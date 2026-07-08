/* viewer_x11.cpp - Linux/X11 port of viewer.cpp: connect to a streamserver,
 * receive length-prefixed scap packets, decode them into an 8bpp palette
 * canvas and paint it in an X11 window (runs under XWayland on Wayland too).
 * Mouse input over the window (move, left/right down/up, wheel) is forwarded
 * to the server as fixed-size ScapInputMsg records for SendInput emulation.
 *
 * Build: g++ -O2 -o viewer viewer_x11.cpp -lX11 -lzstd -lpthread
 * Usage: ./viewer [host]       (host defaults to 127.0.0.1, port SCAP_STREAM_PORT)
 *        ./viewer --selftest   (headless decode check; no X, no network)
 *
 * Same shape as the Windows viewer: a reader thread does the blocking frame
 * reads + decode (decoder guarded by one mutex) and posts dirty rects to the
 * GUI thread over a self-pipe; the GUI thread owns every Xlib call, so no
 * XInitThreads is needed. scapdec's GDI canvas (DIBSection + BitBlt) is
 * replaced by an inline port of its decode loop onto a plain byte buffer;
 * paint converts dirty pixels through kScapPal to 32-bit and XPutImage's them.
 * ponytail: single connection, no reconnect (restart the viewer, same as
 * Windows). Assumes a little-endian TrueColor 24/32bpp visual - true of any
 * modern x86 desktop; exotic visuals would need XVisualInfo-driven conversion.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>
#include <mutex>
#include <thread>
#include <vector>
#include "../common/scap_packet.h"
#include "../common/scap_palette.h"
#include "../common/scap_stream.h"

struct DirtyRect { int x0, y0, x1, y1; }; /* half-open; empty when x1<=x0 */

/* The server reads exactly this many bytes per input record off the socket;
 * a size mismatch would desync every event after the first. */
static_assert(sizeof(ScapInputMsg) == 16, "ScapInputMsg wire size");

/* Decoder state: scapdec.cpp's ScapDec minus the GDI objects. Stride == width
 * (no DIB 4-byte row alignment needed on a plain buffer). The DCtx persists
 * across packets: the server flushes one continuing zstd frame, so packets
 * reference earlier ones as history and must be decoded in order. */
struct Dec
{
    int width = 0, height = 0;
    std::vector<uint8_t> canvas; /* 8bpp palette indices, top-down */
    std::vector<uint8_t> raw;    /* decompress scratch */
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    /* cap window demands at 16MB: headroom over scapenc's windowLog 21 so a
     * future 4K bump (23) needs no viewer change; allocation follows the
     * frame's declared window (~2MiB) */
    Dec() { if (dctx) ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 24); }
    ~Dec() { ZSTD_freeDCtx(dctx); }
};

static Dec           g_dec;
static std::mutex    g_lock;    /* guards g_dec + g_status (decode vs. paint) */
static SOCKET        g_sock = INVALID_SOCKET;
static volatile bool g_running = true;
static const char*   g_host = "127.0.0.1";
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
    if (hdr.magic != SCAP_MAGIC || hdr.rectCount == 0 || hdr.width == 0 ||
        hdr.height == 0)
        return -1;

    if (hdr.width != d->width || hdr.height != d->height)
    {
        d->width = hdr.width;
        d->height = hdr.height;
        d->canvas.assign((size_t)d->width * d->height, 0); /* RGB332 index 0 = black */
    }

    if (d->raw.size() < hdr.rawSize)
        d->raw.resize(hdr.rawSize);

    /* Streaming decode: consume the whole blob, expect exactly rawSize out
     * (the encoder flushed everything for this packet). */
    ZSTD_inBuffer  zin = { (const uint8_t*)packet + sizeof(hdr),
                           size - sizeof(hdr), 0 };
    ZSTD_outBuffer zout = { d->raw.data(), hdr.rawSize, 0 };
    while (zin.pos < zin.size)
    {
        size_t rc = ZSTD_decompressStream(d->dctx, &zout, &zin);
        if (ZSTD_isError(rc) || (zout.pos == zout.size && zin.pos < zin.size))
            return -3;
    }
    if (zout.pos != hdr.rawSize)
        return -3;

    const uint8_t* p = d->raw.data();
    const uint8_t* end = p + hdr.rawSize;
    DirtyRect b = { INT_MAX, INT_MAX, 0, 0 };

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
    g_sock = ConnectTo(g_host, SCAP_STREAM_PORT);
    if (g_sock == INVALID_SOCKET)
    {
        SetStatus("connect failed");
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

/* Headless check of the decode path: two packets flushed from one streaming
 * CCtx (packet 2 continues packet 1's zstd frame, so decoding it proves the
 * DCtx persists across packets), verify pixels, bounds and malformed-packet
 * rejection. */
static int SelfTest(void)
{
    std::vector<uint8_t> payload;
    const ScapRectHdr r1 = { 1, 1, 3, 2 }, r2 = { 0, 0, 2, 1 };
    payload.insert(payload.end(), (const uint8_t*)&r1, (const uint8_t*)(&r1 + 1));
    payload.insert(payload.end(), (size_t)r1.w * r1.h, 7);
    payload.insert(payload.end(), (const uint8_t*)&r2, (const uint8_t*)(&r2 + 1));
    payload.insert(payload.end(), (size_t)r2.w * r2.h, 3);

    ScapFrameHdr hdr = { SCAP_MAGIC, 8, 5, 2, 0, (uint32_t)payload.size() };
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    assert(cctx);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    auto build = [&](std::vector<uint8_t>& pkt) {
        pkt.resize(sizeof(hdr) + ZSTD_compressBound(payload.size()));
        ZSTD_inBuffer in = { payload.data(), payload.size(), 0 };
        ZSTD_outBuffer out = { pkt.data() + sizeof(hdr),
                               pkt.size() - sizeof(hdr), 0 };
        size_t rem = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush);
        assert(!ZSTD_isError(rem) && rem == 0 && in.pos == in.size);
        memcpy(pkt.data(), &hdr, sizeof(hdr));
        pkt.resize(sizeof(hdr) + out.pos);
    };
    std::vector<uint8_t> pkt1, pkt2;
    build(pkt1);
    build(pkt2);
    ZSTD_freeCCtx(cctx);

    Dec d;
    DirtyRect b;
    assert(DecodePacket(&d, pkt1.data(), (unsigned)pkt1.size(), &b) == 0);
    assert(d.width == 8 && d.height == 5);
    assert(d.canvas[1 * 8 + 1] == 7 && d.canvas[2 * 8 + 3] == 7); /* r1 */
    assert(d.canvas[0] == 3 && d.canvas[1] == 3);                 /* r2 */
    assert(d.canvas[4 * 8 + 7] == 0);              /* untouched = black (RGB332) */
    assert(b.x0 == 0 && b.y0 == 0 && b.x1 == 4 && b.y1 == 3);
    /* packet 2 is mid-frame: decodable only with the same DCtx, in order */
    assert(DecodePacket(&d, pkt2.data(), (unsigned)pkt2.size(), &b) == 0);
    assert(d.canvas[1 * 8 + 1] == 7 && d.canvas[0] == 3);
    assert(DecodePacket(&d, pkt1.data(), sizeof(hdr) - 1, &b) != 0); /* truncated */
    printf("selftest ok\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return SelfTest();
    if (argc > 1)
        g_host = argv[1];

    signal(SIGPIPE, SIG_IGN); /* pipe/socket errors come back as EPIPE */
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
                     "scap viewer [%s] - %u fps, last pkt %u B, "
                     "avg %u B/pkt %.1f KB/s (3s), %s",
                     g_host, (unsigned)(frames * 1000 / elapsed), g_lastPktSize,
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
