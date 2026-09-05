# Avara on the web (Emscripten)

Builds `src/` to WebAssembly and runs it in a browser on WebGL2. The renderer
already had a GLES 3.0 path (`AVARA_GLES`, used by the iOS target) and WebGL2
*is* GLES 3.0, so the graphics work is mostly reuse.

## Build and run with Docker

Needs only git and docker on the host -- no SDK, no toolchain:

```sh
git clone -b claude/avara-web-multiplayer-clone-l1w955 https://github.com/cantino/avara
cd avara
docker compose up --build      # or: make web-docker
```

Then open <http://localhost:8099>.

The image compiles the client with the Emscripten SDK and serves the result
with nginx. A first build takes several minutes; object files live in a
BuildKit cache mount, so editing one source file and running the same command
again recompiles only that file.

```sh
AVARA_WEB_PORT=3000 docker compose up --build          # different port
AVARA_WEB_LEVELSETS="blockparty wut" docker compose up --build   # other level sets
```

## Build with a local toolchain

```sh
# once
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest

# then, from the repo root
make web            # or: platform/web/build.sh
```

Output lands in `build-web/`: `avara.html`, `avara.js`, `avara.wasm`,
`avara.data`.

`EMSDK` overrides the SDK location. `AVARA_WEB_LEVELSETS` picks which level
sets get packaged, space separated; the default is the six original sets
(`aa-normal aa-abnormal aa-deux-normal aa-deux-abnormal aa-tre single-player`),
about 3MB and 136 levels. The full 74-set corpus is far too large to preload,
and serving sets over HTTP is future work.

If the SDL2 port cannot be downloaded (it comes from a GitHub archive zip),
clone the matching tag and point the build at it:

```sh
git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL ~/ports/SDL
SDL2_PORT_DIR=~/ports/SDL make web
```

## Multiplayer

Browsers cannot open UDP sockets, so the client tunnels its datagrams to a
gateway (`platform/web/gateway`) over a WebSocket. The gateway hands each
session a virtual IPv4 address out of `10.0.0.0/8` and relays datagrams between
sessions by that address, so `CUDPComm` keeps working with ordinary `IPaddress`
values and never learns that no UDP is involved. `docker compose up` runs it
alongside nginx, which proxies `/net` to it.

The low 24 bits of the address are shown as a five-character **room code**.

| | |
|---|---|
| Host | `avara.html?host=1` -- the room code appears in the bar at the bottom |
| Join | `avara.html?join=CODE`, or type the code into the Address box |
| Invite | the **copy invite link** button builds the `?join=` URL |

A gateway elsewhere: `?gateway=wss://host/net`.

### Cross-origin isolation is required

The page must be served with

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`platform/web/nginx.conf` sets both, so `docker compose up` is fine. Without
them `SharedArrayBuffer` is unavailable, the transport falls back to a
WebSocket on the main thread, and **matches cannot start** -- the client logs a
warning saying so. Single player and the lobby still work.

The reason is that the engine's connect, gather-players and resume paths all
spin in place waiting for the other players to answer. Nothing can be delivered
to a main-thread socket while such a loop runs, so they always time out. So the
socket lives on a worker and datagrams cross into the main thread through a
`SharedArrayBuffer` ring that can be read synchronously, which makes receiving
behave like a real UDP socket and lets those loops work unchanged.

### Playing with people who are not on your machine

The page must be a **secure context** for `SharedArrayBuffer` to exist at all:
`https://`, or `http://localhost`. Serving the game to friends over plain HTTP
on a LAN address will load and let everyone into the lobby, and then no match
will start. So put it behind TLS -- a tunnel (`cloudflared tunnel --url
http://localhost:8099`), a reverse proxy with a certificate, or any host you
already have -- and share that URL. The WebSocket follows the page's scheme, so
`wss://` needs no separate setup.

One host runs both nginx and the gateway; everyone connects out to it, which is
the point of the design -- no player needs an open inbound port, and neither
does the NAT traversal Avara normally relies on.

### Starting a match

1. Host opens `avara.html?host=1` and sends the invite link.
2. Once everyone is in the roster, the host picks a set and level and clicks
   **load level**. This is what syncs everyone: a client that joins later keeps
   whatever level it picked at startup until the host loads one.
3. Everyone clicks **ready &#8730;** (or the host just clicks **start match**).

The text box in the bar is chat, and anything starting with `/` is a command --
`/help` lists them. `/load chok`, `/random`, `/teams`, `/kick 3`, `/away` and
the rest all work, because it goes through the same roster-message path the
in-canvas chat uses.

## Run

Serve `build-web/` over HTTP -- opening `avara.html` from `file://` will not
work, because the `.wasm` and `.data` files are fetched.

It also needs the two cross-origin isolation headers above, which
`python3 -m http.server` does not send, so use the small server next to it:

```sh
platform/web/serve.py 8099 build-web
# then open http://localhost:8099/avara.html
```

`JOBS` controls compile parallelism (defaults to the CPU count).

Query-string options, so a link can pick what loads:

| Parameter | Effect |
|---|---|
| `?name=Andrew` | sets the player name |
| `?cmd=/load%20alektra` | runs a chat command at startup (repeatable) |
| `?frametime=64` | classic 64 ms tick instead of the default 16 ms |
| `?host=1` | start hosting |
| `?join=CODE` | join a room |
| `?gateway=URL` | use a gateway other than `/net` on this origin |
| `?arg=-s` | passes a raw command-line argument (repeatable) |

Alt-click **Start/Ready** to start a solo game immediately rather than only
sending a ready checkmark.

## What differs from a native build

| Area | Web build |
|---|---|
| Transport | `src/net/AvaraTCPWeb.cpp` replaces `AvaraTCP.cpp`, tunnelling datagrams to the gateway over a WebSocket. Everything above it (`CUDPComm`, `CNetManager`) is unchanged |
| Connecting | `CUDPComm::ContactServer` blocks waiting for the server's reply, which in a browser can only ever deadlock and time out -- nothing arrives while wasm holds the thread. On the web it sends the login and returns; `CNetManager::PumpPendingNet` adopts the connection from the frame loop once the server has assigned a slot |
| Audio device | Opened once for the life of the page. `CAvaraGame::InitMixer` disposes and recreates the mixer on every level load, and emscripten's SDL2 does not disconnect its `ScriptProcessorNode` synchronously, so closing the device left the callback reading freed memory |
| Tracker | Not available. `cpp-httplib` needs raw sockets and `TrackerPinger` needs a background thread; both need replacing with `emscripten_fetch`, and reading the game list from a browser also needs CORS on the tracker |
| Assets | Preloaded into MEMFS at `/`, which is what `SDL_GetBasePath()` returns here, so `GetBasePath()` resolves `rsrc/` and `levels/` unchanged |
| Main loop | `emscripten_set_main_loop` on `requestAnimationFrame`; the browser owns the frame clock, so the loop never blocks waiting for input. While the tab is hidden it gets no animation frames at all, which in lockstep would stall every other player, so a worker clock drives `nanogui::pump_mainloop()` instead -- simulation and network without rendering |
| Canvas | Sized by the page to the window, so the game renders at the window's resolution rather than being scaled from a fixed buffer. `SDL_SetWindowSize` from `avara_web_resize`, which nanogui already handles |
| Page controls | Level picker, ready, start and a chat/command box live in the page rather than in the canvas. Chat goes through `CNetManager::SendRosterMessage`, the same path the in-canvas roster uses, so all the `/` commands work |
| GUI | nanogui, unchanged, inside the canvas |
| `vendor/nanogui/glutil.cpp` | Excluded. Desktop-GL only, and nothing in Avara references it |

## Known gaps

- **Cross-play with native clients.** The gateway relays only between its own
  sessions; a real UDP leg would let native players join, and needs no client
  change. Determinism across the two toolchains is unverified.
- **Tracker.** No game list, and no registration.
- **Level sets are baked into the build.** 136 levels, no way to add more
  without rebuilding.
- **Audio needs a click first**, per browser autoplay policy. The shell resumes
  the SDL2 audio context on the first canvas click.
- **5.8MB of wasm, uncompressed on the wire past gzip.** Brotli would roughly
  halve it.
- The background-tab clock is verified only by driving `visibilitychange`
  by hand: headless Chromium reports every page visible, so real throttling
  could not be reproduced here.
