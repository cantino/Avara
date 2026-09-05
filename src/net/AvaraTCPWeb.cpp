/*
    Web (Emscripten) implementation of the platform transport layer.

    Browsers cannot open UDP sockets, so the BSD-socket implementation in
    AvaraTCP.cpp cannot be used. Everything above this file -- CUDPComm,
    CUDPConnection, CCommManager -- is platform independent and unchanged;
    this is the only layer that differs.

    Datagrams are tunnelled to a gateway over a WebSocket. The gateway hands
    each session a virtual IPv4 address and relays datagrams between sessions
    by that address, so CUDPComm keeps working with ordinary IPaddress values
    and never learns that no UDP is involved. A gateway that also owns a real
    UDP socket can relay to native clients the same way, without changing
    anything here.

    Wire format, both directions, over binary WebSocket frames:

        0x01 HELLO    [port:2]                     client -> gateway
        0x02 DATA     [ip:4][port:2][payload...]   client -> gateway
        0x81 WELCOME  [ip:4][port:2]               gateway -> client
        0x82 DATA     [ip:4][port:2][payload...]   gateway -> client

    Addresses on the wire here are big-endian, matching what IPaddress holds.

    The WebSocket itself lives on a worker (platform/web/net.js) and hands
    datagrams over through a SharedArrayBuffer ring, so CheckSockets can read
    them without returning to the browser's event loop. The engine's connect,
    gather and resume paths all block waiting for the other players, and
    nothing can be delivered to a main-thread socket while they do.
*/

#include "AvaraTCP.h"

#include <SDL2/SDL.h>
#include <emscripten.h>

#include <cstdlib>
#include <cstring>
#include <string>

typedef struct {
    ReadCompleteProc *callback;
    void *userData;
} UDPReadData;

static Boolean gAvaraTCPOpen = false;
static int gNextSocket = 1;
static UDPReadData gReadCallback = {NULL, NULL};
static PunchHandler gPunchHandler;

// ---------------------------------------------------------------------------
// The JavaScript side lives in platform/web/net.js, linked in with --pre-js.
// It owns the WebSocket -- on a worker, with a SharedArrayBuffer ring, so that
// receiving is synchronous and the engine's blocking waits work.
// ---------------------------------------------------------------------------

EM_JS(void, avara_net_bind, (int port), { AvaraNet.bind(port); });
EM_JS(void, avara_net_unbind, (), { AvaraNet.unbind(); });
EM_JS(int, avara_net_status, (), { return AvaraNet.status(); });
EM_JS(unsigned int, avara_net_local_host, (), { return AvaraNet.localHost(); });

EM_JS(void, avara_net_send, (unsigned int host, unsigned int port, const char *data, int len), {
    AvaraNet.send(host >>> 0, port, HEAPU8.subarray(data, data + len));
});

// Returns the payload length, or -1 when nothing is queued. The source
// address is written to srcOut as [ip:4][port:2], big-endian.
EM_JS(int, avara_net_recv, (char *buf, int maxLen, char *srcOut), {
    return AvaraNet.recvInto(HEAPU8, buf, maxLen, srcOut);
});

// ---------------------------------------------------------------------------

OSErr OpenAvaraTCP() {
    if (gAvaraTCPOpen) {
        return noErr;
    }
    SDL_Log("OpenAvaraTCP (web: WebSocket gateway transport)\n");
    gAvaraTCPOpen = true;
    return noErr;
}

int CreateSocket(uint16_t &port) {
    if (port == 0) {
        port = 19567;
    }
    avara_net_bind(port);
    return gNextSocket++;
}

void DestroySocket(int sock) {
    avara_net_unbind();
    gReadCallback.callback = NULL;
    gReadCallback.userData = NULL;
}

UDPpacket *CreatePacket(int bufferSize) {
    UDPpacket *packet = (UDPpacket *)malloc(sizeof(UDPpacket));
    packet->data = (uint8_t *)malloc(bufferSize);
    packet->len = 0;
    packet->data[0] = 0;
    return packet;
}

void FreePacket(UDPpacket *packet) {
    if (packet) {
        free(packet->data);
        free(packet);
    }
}

// Accepts a dotted quad, or a room code as handed out by the gateway (five
// base32 characters standing for the low 24 bits of a 10.x.y.z address).
int ResolveHost(IPaddress *address, const char *host, uint16_t port) {
    address->port = SDL_SwapBE16(port);
    if (host == NULL) {
        // Asking for our own address; the gateway assigned it.
        address->host = SDL_SwapBE32(avara_net_local_host());
        return 0;
    }
    address->host = 0;

    unsigned int a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
        address->host = SDL_SwapBE32((a << 24) | (b << 16) | (c << 8) | d);
        return 0;
    }

    static const char *alpha = "23456789abcdefghjkmnpqrstuvwxyz";
    std::string code(host);
    if (code.size() == 5) {
        uint32_t id = 0;
        for (char ch : code) {
            const char *at = strchr(alpha, tolower((unsigned char)ch));
            if (at == NULL) {
                id = 0;
                break;
            }
            id = id * 31 + (uint32_t)(at - alpha);
        }
        if (id != 0) {
            address->host = SDL_SwapBE32(0x0A000000u | (id & 0xffffff));
            return 0;
        }
    }

    SDL_Log("ResolveHost(%s): not a dotted quad or a room code\n", host);
    return -1;
}

void PunchSetup(const char *host, uint16_t port) {}
void RegisterPunchServer(IPaddress &localAddr) {}
void RequestPunch(IPaddress &addr) {}
void PunchHole(const IPaddress &addr, const int8_t connectionId) {}

void CheckSockets() {
    if (!gAvaraTCPOpen || gReadCallback.callback == NULL) {
        return;
    }

    static uint8_t src[6];
    UDPpacket *packet = CreatePacket(UDPSTREAMBUFFERSIZE);
    for (;;) {
        int len = avara_net_recv((char *)packet->data, UDPSTREAMBUFFERSIZE, (char *)src);
        if (len < 0) {
            break;
        }
        packet->len = (uint32_t)len;
        memcpy(&packet->address.host, src, 4);
        memcpy(&packet->address.port, src + 4, 2);
        if (len > 0) {
            gReadCallback.callback(packet, gReadCallback.userData);
        }
    }
    FreePacket(packet);
}

void UDPRead(int sock, ReadCompleteProc callback, void *userData) {
    gReadCallback.callback = callback;
    gReadCallback.userData = userData;
}

void UDPWrite(int sock, UDPpacket *packet, WriteCompleteProc callback, void *userData) {
    avara_net_send(SDL_SwapBE32(packet->address.host), SDL_SwapBE16(packet->address.port),
                   (const char *)packet->data, (int)packet->len);
    if (callback) {
        callback(0, userData);
    }
}

std::string FormatHostPort(uint32_t host, uint16_t port) {
    uint32_t h = SDL_SwapBE32(host);
    uint16_t p = SDL_SwapBE16(port);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
             (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff, p);
    return std::string(buf);
}

std::string FormatAddress(const IPaddress &addr) {
    return FormatHostPort(addr.host, addr.port);
}

void SetPunchMessageHandler(PunchHandler handler) {
    gPunchHandler = handler;
}
