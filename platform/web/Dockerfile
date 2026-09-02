# syntax=docker/dockerfile:1
#
# Builds the WebAssembly client and serves it, so the only things needed on the
# host are git and docker.
#
#   docker compose up --build     # then open http://localhost:8099
#
# See platform/web/README.md.

# Overridable so a mirror or a locally patched base can be substituted.
ARG EMSDK_IMAGE=emscripten/emsdk:6.0.9
ARG NGINX_IMAGE=nginx:1.29-alpine

FROM ${EMSDK_IMAGE} AS build

# Emscripten fetches its SDL2 port from a GitHub archive zip, which some
# networks block even when git itself works. Clone the matching tag instead.
# The version is read out of the SDK in this image, so the two cannot drift.
RUN set -eux; \
    sdl_version="$(sed -n "s/^VERSION = '\([^']*\)'.*/\1/p" \
        "$EMSDK/upstream/emscripten/tools/ports/sdl2.py")"; \
    git clone --depth 1 --branch "release-${sdl_version}" \
        https://github.com/libsdl-org/SDL /opt/ports/SDL
ENV SDL2_PORT_DIR=/opt/ports/SDL

WORKDIR /src
COPY . .

# Which level set gets packaged into avara.data. The full corpus is far too
# large to preload; serving sets over HTTP is future work.
ARG AVARA_WEB_LEVELSET=aa-normal
ENV AVARA_WEB_LEVELSET=${AVARA_WEB_LEVELSET}

# The object files live in a cache mount so that editing one source file does
# not recompile all ~160 of them; the artifacts are copied out into the layer.
RUN --mount=type=cache,target=/objects,sharing=locked \
    BUILD_DIR=/objects platform/web/build.sh \
    && mkdir -p /out && cp /objects/avara.data /objects/avara.html \
                          /objects/avara.js /objects/avara.wasm /out/

FROM ${NGINX_IMAGE} AS serve
COPY --from=build /out/ /usr/share/nginx/html/
COPY platform/web/nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
