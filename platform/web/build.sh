#!/usr/bin/env bash
# Emscripten build for Avara. Run from the repo root:  platform/web/build.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
# Use an already-activated SDK if the caller sourced emsdk_env.sh, otherwise
# look in the usual place. Override with EMSDK=/path/to/emsdk.
if ! command -v em++ >/dev/null 2>&1; then
  EMSDK=${EMSDK:-$HOME/emsdk}
  if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
    echo "emscripten not found. Install it, or set EMSDK=/path/to/emsdk." >&2
    echo "  git clone https://github.com/emscripten-core/emsdk ~/emsdk" >&2
    echo "  ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest" >&2
    exit 1
  fi
  source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
fi

# The SDL2 port is normally fetched from a GitHub archive zip. Where that is
# unreachable, point emscripten at a local checkout of the same tag instead.
if [ -n "${SDL2_PORT_DIR:-}" ] && [ -d "$SDL2_PORT_DIR" ]; then
  export EMCC_LOCAL_PORTS="sdl2=$SDL2_PORT_DIR"
fi

BUILD_DIR=${BUILD_DIR:-build-web}
mkdir -p "$BUILD_DIR"

# Stamp the build. Tolerate a source tree with no git metadata (a tarball, or
# a container build that excluded .git).
GITV=$(git describe --always --dirty 2>/dev/null || echo unknown)
grep -q "$GITV" src/util/GitVersion.h 2>/dev/null || \
  echo "#define GIT_VERSION \"$GITV\"" > src/util/GitVersion.h

SRC_DIRS=$(find src -type d -not -path src)
SRC_DIRS="$SRC_DIRS vendor/nanovg vendor/nanogui vendor/pugixml vendor"
INCFLAGS=""
for d in $SRC_DIRS; do INCFLAGS="$INCFLAGS -I$d"; done

OPTFLAGS=${OPTFLAGS:--O2 -g1}
CPPFLAGS="$INCFLAGS -MMD -MP -Wall -DAVARA_GLES $OPTFLAGS"
CPPFLAGS="$CPPFLAGS -sUSE_SDL=2 -sUSE_SQLITE3=1"
CPPFLAGS="$CPPFLAGS -Wno-unknown-pragmas -Wno-multichar -Wno-deprecated-declarations"
CXXFLAGS="-std=c++17"

SRCS=$(for d in $SRC_DIRS; do find "$d" -maxdepth 1 \( -name '*.cpp' -o -name '*.c' \) ; done)
# nanogui's glutil (GLShader/GLFramebuffer helpers) is desktop-GL only and
# nothing in Avara references it -- Avara has its own OpenGLShader.
SRCS=$(echo "$SRCS" | grep -v 'vendor/nanogui/glutil.cpp')
# Browsers have no UDP sockets; AvaraTCPWeb.cpp replaces the BSD-socket layer.
SRCS=$(echo "$SRCS" | grep -v 'src/net/AvaraTCP.cpp')

# Compile in parallel. xargs -P rather than bash job control so this still
# works on the bash 3.2 that ships with macOS.
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

compile_one() {
  src="$1"
  obj="$BUILD_DIR/$src.o"
  mkdir -p "$(dirname "$obj")"
  # Skip anything already newer than its source.
  if [ -f "$obj" ] && [ ! "$src" -nt "$obj" ]; then return 0; fi
  case "$src" in
    *.c)   emcc $CPPFLAGS -c "$src" -o "$obj" 2> "$obj.log" ;;
    *.cpp) em++ $CPPFLAGS $CXXFLAGS -c "$src" -o "$obj" 2> "$obj.log" ;;
  esac || { echo "FAILED: $src"; touch "$BUILD_DIR/.failed"; }
}
export -f compile_one
export BUILD_DIR CPPFLAGS CXXFLAGS

rm -f "$BUILD_DIR/.failed"
echo "compiling $(echo "$SRCS" | wc -w | tr -d ' ') sources with $JOBS jobs..."
printf '%s\n' $SRCS | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

if [ -f "$BUILD_DIR/.failed" ]; then
  for src in $SRCS; do
    if [ -s "$BUILD_DIR/$src.o.log" ] && grep -q "error:" "$BUILD_DIR/$src.o.log"; then
      echo "--- $src ---"
      grep "error:" "$BUILD_DIR/$src.o.log" | head -5
    fi
  done
  exit 1
fi

OBJS=""
for src in $SRCS; do OBJS="$OBJS $BUILD_DIR/$src.o"; done
echo "$OBJS" > "$BUILD_DIR/objs.txt"

# --- link ---
em++ $CPPFLAGS $CXXFLAGS -c src/Avara.cpp -o "$BUILD_DIR/src/Avara.cpp.o" 2>&1 | tail -20

LDFLAGS="-sUSE_SDL=2 -sUSE_SQLITE3=1"
LDFLAGS="$LDFLAGS -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1"
LDFLAGS="$LDFLAGS -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=5242880"
LDFLAGS="$LDFLAGS -sEXIT_RUNTIME=0 -sASSERTIONS=1 -sNO_DISABLE_EXCEPTION_CATCHING"
LDFLAGS="$LDFLAGS -sEXPORTED_RUNTIME_METHODS=['callMain','ccall','cwrap']"
LDFLAGS="$LDFLAGS $OPTFLAGS"

# Assets are preloaded into MEMFS at "/", which is what SDL_GetBasePath()
# returns here, so GetBasePath() resolves rsrc/ and levels/ unchanged.
PRELOAD="--preload-file rsrc/bsps@/rsrc/bsps"
PRELOAD="$PRELOAD --preload-file rsrc/ogg@/rsrc/ogg"
PRELOAD="$PRELOAD --preload-file rsrc/img@/rsrc/img"
PRELOAD="$PRELOAD --preload-file rsrc/shaders@/rsrc/shaders"
PRELOAD="$PRELOAD --preload-file rsrc/objects.json@/rsrc/objects.json"
PRELOAD="$PRELOAD --preload-file rsrc/set.json@/rsrc/set.json"
PRELOAD="$PRELOAD --preload-file rsrc/default.avarascript@/rsrc/default.avarascript"
PRELOAD="$PRELOAD --preload-file levels/${AVARA_WEB_LEVELSET:-aa-normal}@/levels/${AVARA_WEB_LEVELSET:-aa-normal}"

echo "linking..."
em++ $(cat "$BUILD_DIR/objs.txt") "$BUILD_DIR/src/Avara.cpp.o" \
  -o "$BUILD_DIR/avara.html" $LDFLAGS $PRELOAD --shell-file platform/web/shell.html
echo "built: $BUILD_DIR/avara.html"
ls -lh "$BUILD_DIR"/avara.* 2>/dev/null
