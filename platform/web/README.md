# Avara on the web (Emscripten)

Builds `src/` to WebAssembly and runs it in a browser on WebGL2. The renderer
already had a GLES 3.0 path (`AVARA_GLES`, used by the iOS target) and WebGL2
*is* GLES 3.0, so the graphics work is mostly reuse.

## Build

```sh
# once
git clone https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest

# then, from the repo root
make web            # or: platform/web/build.sh
```

Output lands in `build-web/`: `avara.html`, `avara.js`, `avara.wasm`,
`avara.data`.

`EMSDK` overrides the SDK location. `AVARA_WEB_LEVELSET` picks which level set
gets packaged (default `aa-normal`); the full 74-set corpus is far too large to
preload, and serving sets over HTTP is future work.

If the SDL2 port cannot be downloaded (it comes from a GitHub archive zip),
clone the matching tag and point the build at it:

```sh
git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL ~/ports/SDL
SDL2_PORT_DIR=~/ports/SDL make web
```

## Run

Serve `build-web/` over HTTP -- opening `avara.html` from `file://` will not
work, because the `.wasm` and `.data` files are fetched.

```sh
cd build-web && python3 -m http.server 8099
# then open http://localhost:8099/avara.html
```

Query-string options, so a link can pick what loads:

| Parameter | Effect |
|---|---|
| `?name=Andrew` | sets the player name |
| `?cmd=/load%20alektra` | runs a chat command at startup (repeatable) |
| `?frametime=64` | classic 64 ms tick instead of the default 16 ms |
| `?arg=-s` | passes a raw command-line argument (repeatable) |

Alt-click **Start/Ready** to start a solo game immediately rather than only
sending a ready checkmark.

## What differs from a native build

| Area | Web build |
|---|---|
| Transport | `src/net/AvaraTCPWeb.cpp` replaces `AvaraTCP.cpp`. Browsers have no UDP sockets, so this is currently a **null transport**: single-player and `kNullNet` work, real networking does not. Everything above it (`CUDPComm`, `CNetManager`) is unchanged |
| Tracker | Not available. `cpp-httplib` needs raw sockets and `TrackerPinger` needs a background thread; both need replacing with `emscripten_fetch`, and reading the game list from a browser also needs CORS on the tracker |
| Assets | Preloaded into MEMFS at `/`, which is what `SDL_GetBasePath()` returns here, so `GetBasePath()` resolves `rsrc/` and `levels/` unchanged |
| Main loop | `emscripten_set_main_loop` on `requestAnimationFrame`; the browser owns the frame clock, so the loop never blocks waiting for input |
| GUI | nanogui, unchanged, inside the canvas |
| `vendor/nanogui/glutil.cpp` | Excluded. Desktop-GL only, and nothing in Avara references it |

## Known gaps

- **No multiplayer.** The transport is a stub. This is the next piece of work.
- **Fixed 1024x768 canvas.** It does not follow the window; the canvas is
  centered on black instead.
- **One tick per rendered frame.** Same as the desktop loop, but it means a
  client that cannot render at the tick rate runs the simulation slow. A
  background browser tab is the acute case (`requestAnimationFrame` drops to
  roughly 1 Hz) and will need explicit handling before multiplayer, since in
  lockstep one stalled peer stalls the match.
- **Audio needs a click first**, per browser autoplay policy. The shell resumes
  the SDL2 audio context on the first canvas click.
