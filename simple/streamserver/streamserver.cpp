/* streamserver.cpp - capture the primary desktop via scapenc and stream the
 * encoded packets to a connected viewer over TCP (length-prefixed frames).
 * The viewer's mouse input flows back on the same socket (fixed ScapInputMsg)
 * and is emulated here with SendInput.
 *
 * Listens on TCP SCAP_STREAM_PORT (44300). One client at a time: while a client
 * is connected the server polls scapenc and forwards every produced packet, and
 * a per-client thread receives input messages and injects them; when the client
 * disconnects it loops back to accept the next one.
 * ponytail: single-client, serial accept loop. A second connector waits in the
 * listen backlog until the first drops. Upgrade path: spawn a thread per accept
 * and give each its own ScapEnc if concurrent viewers are ever needed.
 */
#include <stdio.h>
#include <atomic>
#include <thread>
#include <vector>
#include "scapenc.h"
#include "scap_stream.h"
/* winsock2.h (via scap_stream.h) is included first; with WIN32_LEAN_AND_MEAN
 * (set in the vcxproj) windows.h then skips the clashing winsock v1 header. */
#include <windows.h>

/* Map one input message to a SendInput MOUSEINPUT. Position is always applied
 * (MOVE|ABSOLUTE) so clicks/wheel land where the viewer's cursor is. Pure
 * function so the mapping is unit-checkable without injecting. Returns the
 * INPUT count (0 = unknown type, ignore). */
static int BuildMouseInput(const ScapInputMsg& m, INPUT* out)
{
    ZeroMemory(out, sizeof(INPUT));
    out->type = INPUT_MOUSE;
    out->mi.dx = (LONG)m.x;
    out->mi.dy = (LONG)m.y;
    DWORD f = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    switch (m.type)
    {
    case SCAP_IN_MOVE:  break;
    case SCAP_IN_LDOWN: f |= MOUSEEVENTF_LEFTDOWN;  break;
    case SCAP_IN_LUP:   f |= MOUSEEVENTF_LEFTUP;    break;
    case SCAP_IN_RDOWN: f |= MOUSEEVENTF_RIGHTDOWN; break;
    case SCAP_IN_RUP:   f |= MOUSEEVENTF_RIGHTUP;   break;
    case SCAP_IN_WHEEL: f |= MOUSEEVENTF_WHEEL; out->mi.mouseData = (DWORD)m.wheel; break;
    default: return 0;
    }
    out->mi.dwFlags = f;
    return 1;
}

/* Receive input messages from the client and emulate them until the socket
 * closes. Runs on its own thread (recv direction is independent of the frame
 * send direction, so it shares the socket safely). */
static void InputPump(SOCKET client, std::atomic<bool>& alive)
{
    ScapInputMsg m;
    while (alive.load() && ScapRecvAll(client, &m, sizeof(m)))
    {
        INPUT in;
        if (BuildMouseInput(m, &in))
            SendInput(1, &in, sizeof(INPUT));
    }
    alive.store(false);
}

/* Print this machine's IPv4 addresses so the user knows where to point a viewer
 * (viewer.exe <ip>). Loopback is always usable for a same-PC test. */
static void PrintLocalAddrs(void)
{
    char host[256] = {};
    printf("connect a viewer to one of:\n");
    printf("  127.0.0.1        (this PC)\n");
    if (gethostname(host, sizeof(host)) != 0)
        return;
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0)
        return;
    for (addrinfo* p = res; p; p = p->ai_next)
    {
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip));
        printf("  %-16s (%s)\n", ip, host);
    }
    freeaddrinfo(res);
}

/* Serve one connected client until it disconnects or capture dies. */
static void Serve(ScapEnc* enc, SOCKET client)
{
    std::atomic<bool> alive(true);
    std::thread input(InputPump, client, std::ref(alive));

    while (alive.load())
    {
        const void* pkt;
        unsigned n;
        int rc = ScapEnc_CaptureFrame(enc, 100, &pkt, &n);
        if (rc == SCAPENC_OK)
        {
            /* pkt is valid until the next ScapEnc_* call; send it now. */
            if (!ScapSendFrame(client, pkt, n))
                break; /* client gone / write error */
        }
        else if (rc == SCAPENC_ERR)
        {
            break; /* unrecoverable capture failure */
        }
        /* TIMEOUT / NOCHANGE / AGAIN: nothing to send, keep polling. */
    }

    /* Stop the input thread and unblock its recv so join() returns. */
    alive.store(false);
    shutdown(client, SD_BOTH);
    input.join();
}

static int RunServer(void)
{
    /* Unbuffered stdout so the listen banner and client IPs appear immediately,
     * whether attached to a console or redirected to a pipe/file. */
    setvbuf(stdout, nullptr, _IONBF, 0);

    ScapNetInit net;
    if (!net.ok)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET lsn = socket(AF_INET, SOCK_STREAM, 0);
    if (lsn == INVALID_SOCKET)
    {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        return 1;
    }
    BOOL yes = TRUE;
    setsockopt(lsn, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SCAP_STREAM_PORT);
    if (bind(lsn, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(lsn, 1) != 0)
    {
        fprintf(stderr, "bind/listen on %d failed: %d\n",
                SCAP_STREAM_PORT, WSAGetLastError());
        closesocket(lsn);
        return 1;
    }

    printf("streamserver listening on TCP %d\n", SCAP_STREAM_PORT);
    PrintLocalAddrs(); /* show which IPs a viewer can connect to */
    for (;;)
    {
        sockaddr_in peer = {};
        int plen = sizeof(peer);
        SOCKET client = accept(lsn, (sockaddr*)&peer, &plen);
        if (client == INVALID_SOCKET)
            continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        printf("client connected: %s\n", ip);

        /* The viewer opens with a ScapHello naming the codec; the encoder is
         * created per connection with that spec, so a fresh viewer always
         * starts at a keyframe / full frame by construction.
         * ponytail: the blocking hello recv lets one silent client stall the
         * serial accept loop - same property the frame loop already has. */
        ScapHello hello;
        if (!ScapRecvAll(client, &hello, sizeof(hello)) ||
            hello.magic != SCAP_HELLO_MAGIC)
        {
            printf("no/bad hello, dropping client\n");
            closesocket(client);
            continue;
        }
        hello.codec[sizeof(hello.codec) - 1] = '\0';
        ScapEnc* enc = ScapEnc_Create(hello.codec);
        if (!enc)
        {
            printf("bad codec spec \"%s\" (or no D3D11 device), dropping\n",
                   hello.codec);
            closesocket(client);
            continue;
        }
        printf("codec: %s\n", hello.codec[0] ? hello.codec : "(default)");
        Serve(enc, client);
        ScapEnc_Destroy(enc);
        closesocket(client);
        printf("client disconnected\n");
    }

    /* not reached; OS reclaims lsn on exit */
}

/* Verify the input-message -> SendInput flag mapping without injecting. */
static bool CheckMouseMapping(void)
{
    const DWORD base = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    struct { uint16_t type; DWORD want; } cases[] = {
        { SCAP_IN_MOVE,  base },
        { SCAP_IN_LDOWN, base | MOUSEEVENTF_LEFTDOWN },
        { SCAP_IN_LUP,   base | MOUSEEVENTF_LEFTUP },
        { SCAP_IN_RDOWN, base | MOUSEEVENTF_RIGHTDOWN },
        { SCAP_IN_RUP,   base | MOUSEEVENTF_RIGHTUP },
        { SCAP_IN_WHEEL, base | MOUSEEVENTF_WHEEL },
    };
    for (auto& c : cases)
    {
        ScapInputMsg m = {};
        m.type = c.type;
        m.x = 100; m.y = 200; m.wheel = 120;
        INPUT in;
        if (BuildMouseInput(m, &in) != 1 || in.type != INPUT_MOUSE ||
            in.mi.dx != 100 || in.mi.dy != 200 || in.mi.dwFlags != c.want)
            return false;
        if (c.type == SCAP_IN_WHEEL && in.mi.mouseData != 120)
            return false;
    }
    ScapInputMsg bad = {}; bad.type = 0xFFFF; INPUT tmp;
    return BuildMouseInput(bad, &tmp) == 0; /* unknown type -> ignored */
}

/* Non-interactive check (`streamserver.exe -selftest`): (1) the input->SendInput
 * flag mapping, and (2) a known buffer round-tripped through ScapSendFrame/
 * ScapRecvFrame over a loopback socket pair, confirming the bytes survive the
 * length-prefix framing intact. These are the smallest things that fail if the
 * mouse mapping or the exact-count send/recv loops break. It does not touch
 * scapenc (that pipeline is covered by test.exe -selftest). Exit 0 = pass. */
static int SelfTest(void)
{
    if (!CheckMouseMapping())
        return 5;

    ScapNetInit net;
    if (!net.ok)
        return 1;

    SOCKET lsn = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; /* ephemeral */
    if (bind(lsn, (sockaddr*)&a, sizeof(a)) != 0 || listen(lsn, 1) != 0)
        return 2;
    int alen = sizeof(a);
    if (getsockname(lsn, (sockaddr*)&a, &alen) != 0)
        return 2;

    std::vector<char> payload(1000);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (char)(i * 7 + 3);

    std::thread sender([&]() {
        SOCKET c = accept(lsn, nullptr, nullptr);
        if (c != INVALID_SOCKET)
        {
            ScapSendFrame(c, payload.data(), (uint32_t)payload.size());
            closesocket(c);
        }
    });

    SOCKET cli = socket(AF_INET, SOCK_STREAM, 0);
    int ret = 3;
    if (connect(cli, (sockaddr*)&a, sizeof(a)) == 0)
    {
        std::vector<char> got;
        if (ScapRecvFrame(cli, got) &&
            got.size() == payload.size() &&
            memcmp(got.data(), payload.data(), payload.size()) == 0)
            ret = 0;
    }
    closesocket(cli);
    sender.join();
    closesocket(lsn);
    return ret;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "-selftest") == 0)
            return SelfTest();
    return RunServer();
}
