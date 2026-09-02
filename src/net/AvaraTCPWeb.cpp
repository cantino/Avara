/*
    Web (Emscripten) implementation of the platform transport layer.

    Browsers cannot open UDP sockets, so the BSD-socket implementation in
    AvaraTCP.cpp cannot be used. Everything above this file -- CUDPComm,
    CUDPConnection, CCommManager -- is platform independent and unchanged;
    this is the only layer that differs.

    Right now this is a null transport: sockets are handles with no packet
    plumbing behind them, so single-player and kNullNet sessions work and any
    attempt at real networking silently sends into the void. The datagram
    plumbing (a gateway holding a real UDP socket on this client's behalf,
    reached over WebTransport or a WebSocket) slots in behind the same
    functions without changes above.
*/

#include "AvaraTCP.h"

#include <SDL2/SDL.h>

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

OSErr OpenAvaraTCP() {
    if (gAvaraTCPOpen) {
        return noErr;
    }
    SDL_Log("OpenAvaraTCP (web: null transport)\n");
    gAvaraTCPOpen = true;
    return noErr;
}

int CreateSocket(uint16_t &port) {
    // Hand back a distinct non-negative handle so callers treat the socket as
    // live. Nothing is bound; there is no host port in a browser.
    if (port == 0) {
        port = 19567;
    }
    return gNextSocket++;
}

void DestroySocket(int sock) {}

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

int ResolveHost(IPaddress *address, const char *host, uint16_t port) {
    // No resolver in the browser. A gateway build resolves names on the far
    // side, so this only needs to accept dotted quads.
    address->host = 0;
    address->port = SDL_SwapBE16(port);

    unsigned int a, b, c, d;
    if (host && sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a < 256 && b < 256 && c < 256 && d < 256) {
        address->host = SDL_SwapBE32((a << 24) | (b << 16) | (c << 8) | d);
        return 0;
    }
    SDL_Log("ResolveHost(%s): no resolver in this build\n", host ? host : "(null)");
    return -1;
}

void PunchSetup(const char *host, uint16_t port) {}
void RegisterPunchServer(IPaddress &localAddr) {}
void RequestPunch(IPaddress &addr) {}
void PunchHole(const IPaddress &addr, const int8_t connectionId) {}

void CheckSockets() {}

void UDPRead(int sock, ReadCompleteProc callback, void *userData) {
    gReadCallback.callback = callback;
    gReadCallback.userData = userData;
}

void UDPWrite(int sock, UDPpacket *packet, WriteCompleteProc callback, void *userData) {
    // Report success so the retransmit queues above drain normally.
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
