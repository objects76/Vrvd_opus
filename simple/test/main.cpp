/* main.cpp - loopback test: capture the primary desktop via scapenc, feed the
 * packets to scapdec, paint the decoded 256-color canvas in a window.
 * Title bar shows fps / packet size / capture status.
 */
#include <windows.h>
#include <stdio.h>
#include "scapenc.h"
#include "scapdec.h"

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

/* Non-interactive check (`test.exe -selftest`): capture one frame, decode it,
 * verify sizes/bounds agree. Exit code 0 = pass. This is the smallest thing
 * that fails if the capture->encode->decode pipeline breaks. */
static int SelfTest(void)
{
    ScapEnc* enc = ScapEnc_Create();
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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
    if (strstr(lpCmdLine, "-selftest"))
        return SelfTest();

    g_enc = ScapEnc_Create();
    if (!g_enc)
    {
        MessageBoxA(nullptr, "ScapEnc_Create failed (no D3D11 device?)",
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
