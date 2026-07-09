/* viewer.cpp - TCP client for streamserver: connect to a streamserver, receive
 * length-prefixed scap packets, decode them with scapdec and paint the decoded
 * canvas in a window. The network mirror of test.exe's in-process loopback.
 *
 * Usage: viewer.exe [--addr=host[:port]] [--codec=spec]
 *   --addr  defaults to 10.1.110.27:44300 (port default SCAP_STREAM_PORT)
 *   --codec defaults to "zstd:6"; sent to the server in the opening
 *           ScapHello, which encodes with it ("zstd[:level]",
 *           "av1[:i420|:i444]"). scapdec follows the packets automatically.
 *
 * A reader thread does the blocking frame reads + decode so the GUI message loop
 * never stalls; scapdec (decode on the reader, paint on the GUI thread) is guarded
 * by one critical section. ponytail: single connection, no reconnect. Restart the
 * viewer to reconnect after the server drops.
 */
#include <windows.h>
#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM */
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>
#include "scapdec.h"
#include "config.h" /* SCAP_CODEC_DEFAULT */
#include "scap_stream.h"

static ScapDec*        g_dec;
static CRITICAL_SECTION g_lock;   /* guards g_dec (decode vs. paint) */
static HWND            g_hwnd;
static SOCKET          g_sock = INVALID_SOCKET;
static volatile bool   g_running = true;
static const char*     g_host = "10.1.110.27";
static int             g_port = SCAP_STREAM_PORT;
static char            g_codec[sizeof(ScapHello::codec)] = SCAP_CODEC_DEFAULT;

/* title-bar stats (written by reader thread, read by GUI timer; benign race) */
static unsigned g_frames, g_lastPktSize;
static char     g_status[64] = "connecting";
static DWORD    g_fpsTick;

static SOCKET ConnectTo(const char* host, int port)
{
    char portStr[8];
    _snprintf_s(portStr, _TRUNCATE, "%d", port);
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

static void ReaderThread(void)
{
    g_sock = ConnectTo(g_host, g_port);
    if (g_sock == INVALID_SOCKET)
    {
        strcpy_s(g_status, "connect failed");
        return;
    }
    /* First bytes on the wire: tell the server which codec to encode with. */
    ScapHello hello = {};
    hello.magic = SCAP_HELLO_MAGIC;
    strcpy_s(hello.codec, g_codec);
    if (!ScapSendAll(g_sock, &hello, sizeof(hello)))
    {
        strcpy_s(g_status, "hello send failed");
        return;
    }
    strcpy_s(g_status, "streaming");
    g_fpsTick = GetTickCount();

    std::vector<char> frame;
    while (g_running && ScapRecvFrame(g_sock, frame))
    {
        RECT bounds;
        EnterCriticalSection(&g_lock);
        int rc = ScapDec_DecodePacket(g_dec, frame.data(),
                                      (unsigned)frame.size(), &bounds);
        LeaveCriticalSection(&g_lock);
        if (rc == 0)
        {
            ++g_frames;
            g_lastPktSize = (unsigned)frame.size();
            InvalidateRect(g_hwnd, &bounds, FALSE); /* thread-safe */
        }
    }
    if (g_running)
        strcpy_s(g_status, "disconnected");
}

static void UpdateTitle(void)
{
    DWORD now = GetTickCount();
    DWORD elapsed = now - g_fpsTick;
    if (elapsed < 1000)
        return;
    char buf[160];
    _snprintf_s(buf, _TRUNCATE,
                "scap viewer [%s:%d] - %s, %u fps, last pkt %u B, %s",
                g_host, g_port, g_codec, g_frames * 1000 / elapsed,
                g_lastPktSize, g_status);
    SetWindowTextA(g_hwnd, buf);
    g_frames = 0;
    g_fpsTick = now;
}

/* Map a client-area point to a normalized 0..65535 desktop position (using the
 * decoded canvas size = server desktop size) and send it as one input message.
 * Runs on the GUI thread, the only sender, so sends are serialized; the reader
 * thread only receives, so send/recv share g_sock safely. No-op until connected
 * and a frame has arrived (canvas size known). */
static void SendMouse(uint16_t type, int cx, int cy, int wheel)
{
    if (g_sock == INVALID_SOCKET)
        return;
    int w = 0, h = 0;
    EnterCriticalSection(&g_lock);
    ScapDec_GetSize(g_dec, &w, &h);
    LeaveCriticalSection(&g_lock);
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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TIMER:
        UpdateTitle();
        return 0;
    case WM_MOUSEMOVE:
        SendMouse(SCAP_IN_MOVE, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd); /* keep tracking a drag that leaves the client area */
        SendMouse(SCAP_IN_LDOWN, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;
    case WM_LBUTTONUP:
        SendMouse(SCAP_IN_LUP, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        SendMouse(SCAP_IN_RDOWN, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;
    case WM_RBUTTONUP:
        SendMouse(SCAP_IN_RUP, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL:
    {
        /* WM_MOUSEWHEEL carries screen coords; convert to client. */
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        SendMouse(SCAP_IN_WHEEL, p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        EnterCriticalSection(&g_lock);
        int rc = ScapDec_Paint(g_dec, hdc, 0, 0, &ps.rcPaint);
        LeaveCriticalSection(&g_lock);
        if (rc != 0)
            FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    /* ANSI DefWindowProc: the class is RegisterClassA, so WM_SETTEXT carries an
     * ANSI string (matches test.exe; DefWindowProcW would mangle the title). */
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
    /* scapenc captures physical pixels via DXGI (e.g. 1920x1080). Become DPI
     * aware so this window's client coordinates are physical pixels too: without
     * it a scaled display virtualizes client coords, so the 1:1 canvas paint is
     * DWM-upscaled (blurry) and mouse positions sent to the server are offset. */
    SetProcessDPIAware();

    /* --addr=host[:port] --codec=spec. Tokenize on whitespace and quotes
     * (getaddrinfo rejects a stray trailing space, so a shell that appends
     * one, e.g. Start-Process, must not break the connect). Unknown tokens
     * are ignored. */
    static char hostBuf[256];
    static char cmdBuf[512];
    strncpy_s(cmdBuf, lpCmdLine ? lpCmdLine : "", _TRUNCATE);
    char* ctx = nullptr;
    for (char* t = strtok_s(cmdBuf, " \t\"", &ctx); t;
         t = strtok_s(nullptr, " \t\"", &ctx))
    {
        if (strncmp(t, "--addr=", 7) == 0 && t[7])
        {
            strncpy_s(hostBuf, t + 7, _TRUNCATE);
            if (char* colon = strrchr(hostBuf, ':'))
            {
                *colon = '\0';
                int port = atoi(colon + 1);
                if (port > 0 && port < 65536)
                    g_port = port;
            }
            if (hostBuf[0])
                g_host = hostBuf;
        }
        else if (strncmp(t, "--codec=", 8) == 0 && t[8])
            strncpy_s(g_codec, t + 8, _TRUNCATE);
    }

    ScapNetInit net;
    if (!net.ok)
    {
        MessageBoxA(nullptr, "WSAStartup failed", "scap viewer", MB_ICONERROR);
        return 1;
    }

    InitializeCriticalSection(&g_lock);
    g_dec = ScapDec_Create();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "ScapViewer";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "scap viewer",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1280, 800, nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    SetTimer(g_hwnd, 1, 500, nullptr);

    std::thread reader(ReaderThread);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* Tear down the reader: stop the loop and unblock any pending recv. */
    g_running = false;
    if (g_sock != INVALID_SOCKET)
        shutdown(g_sock, SD_BOTH);
    reader.join();
    if (g_sock != INVALID_SOCKET)
        closesocket(g_sock);

    ScapDec_Destroy(g_dec);
    DeleteCriticalSection(&g_lock);
    return 0;
}
