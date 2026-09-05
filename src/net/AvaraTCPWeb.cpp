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
// JavaScript side: one WebSocket, an outbound queue for sends issued before it
// opens, and an inbound queue drained by CheckSockets().
// ---------------------------------------------------------------------------

// The connection lives for the life of the page, not for the life of a
// CUDPComm. Avara tears its socket down and builds a new one whenever the net
// mode changes, and reconnecting to the gateway each time would churn the
// session -- and with it the room code -- for no reason.
EM_JS(void, avara_net_bind, (int port), {
    var A = Module.__avaraNet;
    if (A) {
        // Already connected: just tell the gateway the new port.
        A.localPort = port;
        if (A.ready) {
            var h = new Uint8Array(3);
            h[0] = 1; h[1] = (port >> 8) & 0xff; h[2] = port & 0xff;
            A.ws.send(h);
        }
        return;
    }
    A = Module.__avaraNet = {
        ws: null, ready: false, outQ: [], inQ: [], localHost: 0, localPort: port
    };

    var url = (typeof window !== "undefined" && window.AVARA_GATEWAY_URL) || null;
    if (!url) {
        var proto = (location.protocol === "https:") ? "wss:" : "ws:";
        url = proto + "//" + location.host + "/net";
    }

    var ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    A.ws = ws;

    ws.onopen = function() {
        var hello = new Uint8Array(3);
        hello[0] = 1;
        hello[1] = (A.localPort >> 8) & 0xff;
        hello[2] = A.localPort & 0xff;
        ws.send(hello);
        A.ready = true;
        for (var i = 0; i < A.outQ.length; i++) { ws.send(A.outQ[i]); }
        A.outQ.length = 0;
    };

    ws.onmessage = function(ev) {
        var b = new Uint8Array(ev.data);
        if (b.length < 1) { return; }
        if (b[0] === 0x81 && b.length >= 7) {
            A.localHost = (b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4]) >>> 0;
            A.localPort = (b[5] << 8) | b[6];
            // Room code: the low 24 bits of the address, base32, so it can be
            // typed into the Address box or passed as ?join=CODE.
            var alpha = "23456789abcdefghjkmnpqrstuvwxyz";
            var id = A.localHost & 0xffffff, code = "";
            for (var i = 0; i < 5; i++) { code = alpha[id % 31] + code; id = Math.floor(id / 31); }
            A.roomCode = code;
            if (typeof window !== "undefined") {
                window.__avaraRoomCode = code;
                window.dispatchEvent(new CustomEvent("avara-room-code", {detail: code}));
            }
        } else if (b[0] === 0x82 && b.length >= 7) {
            A.inQ.push(b);
        }
    };

    ws.onclose = function() { A.ready = false; };
    ws.onerror = function() { A.ready = false; };
});

// Drop anything still queued for the socket being torn down, so it cannot be
// delivered to whatever CUDPComm is built next. The connection itself stays up.
EM_JS(void, avara_net_unbind, (), {
    var A = Module.__avaraNet;
    if (A) { A.inQ.length = 0; A.outQ.length = 0; }
});

EM_JS(int, avara_net_status, (), {
    var A = Module.__avaraNet;
    if (!A) { return -1; }
    return (A.ready ? 100 : 0) + (A.ws ? A.ws.readyState : 9);
});

EM_JS(unsigned int, avara_net_local_host, (), {
    var A = Module.__avaraNet;
    return (A && A.localHost) ? A.localHost : 0;
});

EM_JS(void, avara_net_send, (unsigned int host, unsigned int port, const char *data, int len), {
    var A = Module.__avaraNet;
    if (!A) { return; }
    var frame = new Uint8Array(7 + len);
    frame[0] = 2;
    frame[1] = (host >>> 24) & 0xff; frame[2] = (host >>> 16) & 0xff;
    frame[3] = (host >>> 8) & 0xff;  frame[4] = host & 0xff;
    frame[5] = (port >> 8) & 0xff;   frame[6] = port & 0xff;
    frame.set(HEAPU8.subarray(data, data + len), 7);
    if (A.ready) { A.ws.send(frame); } else if (A.outQ.length < 256) { A.outQ.push(frame); }
});

// Returns the payload length, or -1 when nothing is queued. The source
// address is written to srcOut as [ip:4][port:2], big-endian.
EM_JS(int, avara_net_recv, (char *buf, int maxLen, char *srcOut), {
    var A = Module.__avaraNet;
    if (!A || A.inQ.length === 0) { return -1; }
    var b = A.inQ.shift();
    var len = b.length - 7;
    if (len > maxLen) { len = maxLen; }
    HEAPU8.set(b.subarray(1, 7), srcOut);
    HEAPU8.set(b.subarray(7, 7 + len), buf);
    return len;
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
