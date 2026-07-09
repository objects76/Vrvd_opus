/* scap_stream.h - length-prefixed TCP framing shared by streamserver/viewer.
 *
 * A scap packet (ScapFrameHdr + zstd blob, see scap_packet.h) is self-describing
 * for *decoding* but not for *reading off a stream*: the header carries the
 * uncompressed rawSize, never the on-wire (compressed) length. So each packet is
 * framed on the wire as:
 *
 *     [u32 len][len bytes: the scap packet]
 *
 * len is the full packet length (ScapFrameHdr + blob), little-endian.
 * ponytail: assumes same-endian peers (both ends are x86/x64, always
 * little-endian). A cross-endian peer would need htonl/ntohl here.
 */
#pragma once
#include <stdint.h>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define SD_BOTH SHUT_RDWR
#endif

#define SCAP_STREAM_PORT 44300
/* Reject absurd frame lengths before allocating: a 4K desktop at 8bpp is ~33 MB
 * raw and compresses smaller, so 64 MB is a safe upper bound and stops a desynced
 * or hostile length prefix from forcing a huge allocation. */
#define SCAP_STREAM_MAX  (64u * 1024 * 1024)

/* Handshake: the FIRST thing the viewer sends after connect. codec is the
 * NUL-terminated encoder spec passed to ScapEnc_Create() on the server
 * ("zstd:6", "av1:i420", "av1:i444", ...); empty = server default
 * (config.h SCAP_CODEC_DEFAULT). The server rejects the connection on a bad
 * magic or an unparseable spec - fail loudly, not garbage. */
#define SCAP_HELLO_MAGIC 0x4F4C4853u /* 'S','H','L','O' little-endian */

#pragma pack(push, 1)
typedef struct ScapHello
{
    uint32_t magic;     /* SCAP_HELLO_MAGIC */
    char     codec[28]; /* NUL-terminated codec spec; "" = server default */
} ScapHello;
#pragma pack(pop)

/* Reverse channel: viewer -> server mouse input, emulated on the server via
 * SendInput. Fixed-size messages (no length prefix); the server reads exactly
 * sizeof(ScapInputMsg) each time. Position is normalized 0..65535 over the
 * desktop by the *viewer* (which knows the canvas size), so the server can hand
 * it straight to SendInput with MOUSEEVENTF_ABSOLUTE without knowing the
 * desktop resolution. This is a separate direction from the frame stream, so it
 * coexists with the length-prefixed frame framing on the same socket. */
enum ScapInputType
{
    SCAP_IN_MOVE  = 1,
    SCAP_IN_LDOWN = 2,
    SCAP_IN_LUP   = 3,
    SCAP_IN_RDOWN = 4,
    SCAP_IN_RUP   = 5,
    SCAP_IN_WHEEL = 6
};

#pragma pack(push, 1)
typedef struct ScapInputMsg
{
    uint16_t type;     /* ScapInputType */
    uint16_t reserved; /* 0 */
    uint32_t x, y;     /* normalized absolute position, 0..65535 */
    int32_t  wheel;    /* signed wheel delta (WHEEL_DELTA units); WHEEL only */
} ScapInputMsg;
#pragma pack(pop)

/* RAII Winsock startup/cleanup; declare one instance in main() before any socket.
 * No-op on POSIX, where sockets need no library init. */
struct ScapNetInit
{
    bool ok;
#ifdef _WIN32
    ScapNetInit()  { WSADATA wd; ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0); }
    ~ScapNetInit() { if (ok) WSACleanup(); }
#else
    ScapNetInit() : ok(true) {}
#endif
};

/* Blocking send/recv of an exact byte count. Return false on error or peer close. */
inline bool ScapSendAll(SOCKET s, const void* buf, int len)
{
    const char* p = (const char*)buf;
    while (len > 0)
    {
        int n = send(s, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= n;
    }
    return true;
}

inline bool ScapRecvAll(SOCKET s, void* buf, int len)
{
    char* p = (char*)buf;
    while (len > 0)
    {
        int n = recv(s, p, len, 0);
        if (n <= 0) /* 0 = orderly close, <0 = error */
            return false;
        p += n;
        len -= n;
    }
    return true;
}

/* Send one framed packet. len is the packet byte count. */
inline bool ScapSendFrame(SOCKET s, const void* pkt, uint32_t len)
{
    return ScapSendAll(s, &len, sizeof(len)) && ScapSendAll(s, pkt, (int)len);
}

/* Receive one framed packet into out. Returns false on peer close, socket error,
 * or an out-of-range length prefix. */
inline bool ScapRecvFrame(SOCKET s, std::vector<char>& out)
{
    uint32_t len;
    if (!ScapRecvAll(s, &len, sizeof(len)))
        return false;
    if (len == 0 || len > SCAP_STREAM_MAX)
        return false;
    out.resize(len);
    return ScapRecvAll(s, out.data(), (int)len);
}
