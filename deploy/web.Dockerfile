# syntax=docker/dockerfile:1
#
# Web build of the game, served by Caddy (which also forwards /ws to the relay).
# Build context: the repository root.
#
# Targets:
#   web (default)  compiles the game from source inside Docker. This is what
#                  `deploy.sh --build` uses; the first build takes a while
#                  (vcpkg compiles SDL and friends for wasm).
#   web-prebuilt   packs a build that already sits in deploy/web-dist/ (the
#                  output of `cmake --install build/emscripten`). CI uses it
#                  after its regular Emscripten build.

ARG EMSDK_VERSION=6.0.12

FROM emscripten/emsdk:${EMSDK_VERSION} AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends ninja-build pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake --preset emscripten \
 && cmake --build build/emscripten --config Release \
 && cmake --install build/emscripten --config Release --prefix /out

FROM caddy:2-alpine AS base
COPY deploy/Caddyfile /etc/caddy/Caddyfile
COPY deploy/web-entrypoint.sh /usr/local/bin/web-entrypoint.sh
COPY src/web/og.png /srv/og.png
ENTRYPOINT ["/usr/local/bin/web-entrypoint.sh"]

FROM base AS web-prebuilt
COPY deploy/web-dist/ /srv/
RUN mv /srv/openliero.html /srv/page.template.html

FROM base AS web
COPY --from=build /out/ /srv/
RUN mv /srv/openliero.html /srv/page.template.html
