# Project Pixel's platform as a container image (docs/deployment.md): the
# platform service with its game server and checker, built from these
# sources. Two images:
#
#   docker build -t pixel-platform .                         (no browser player)
#   docker build --target platform-web -t pixel-platform .   (with the WebAssembly
#                                                             player: /play runs games)
#
# Run it with deploy/compose.yaml, or by hand:
#
#   docker run -d --name pixel -v pixel-data:/data -p 8730:8730 \
#     -p 27960-27979:27960-27979/udp -p 27960-27979:27960-27979/tcp \
#     pixel-platform --public-host play.example.com
#
# Everything the platform keeps is in /data (a volume): the database, the
# platform's key, packages, pictures, logs and backups.

ARG UBUNTU=ubuntu:24.04
ARG EMSDK_VERSION=6.0.13

# --- The programs ------------------------------------------------------------------------------
FROM ${UBUNTU} AS build
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates cmake ninja-build g++ python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPX_BUILD_CLIENT=OFF -DPX_BUILD_TESTS=OFF \
 && cmake --build /build --target px_platform px_server px_headless \
 && mkdir -p /out \
 && cp /build/services/platform/px-platform /build/apps/server/px-server /build/apps/headless/px-headless /out/ \
 && strip /out/*

# --- What every image has ------------------------------------------------------------------------
FROM ${UBUNTU} AS platform-base
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ca-certificates tzdata \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid 10001 pixel \
 && useradd --system --uid 10001 --gid pixel --home-dir /data --no-create-home --shell /usr/sbin/nologin pixel \
 && mkdir -p /data && chown pixel:pixel /data && chmod 700 /data
COPY --from=build /out/ /opt/pixel/
ENV PATH=/opt/pixel:$PATH
USER pixel
WORKDIR /data
VOLUME /data
# 8730: the API, /play, /healthz and /metrics (HTTP); 8731: the same over
# HTTPS when given a certificate; 27960-27979: game servers (UDP for the apps,
# TCP WebSockets for browsers).
EXPOSE 8730/tcp 8731/tcp 27960-27979/udp 27960-27979/tcp
HEALTHCHECK --interval=30s --timeout=10s --start-period=20s --retries=3 CMD ["px-platform", "--check-health"]
ENTRYPOINT ["px-platform", "--data", "/data", "--bind", "0.0.0.0", "--port", "8730"]
CMD ["--server-ports", "27960:20", "--max-servers", "20"]

# --- The browser player (WebAssembly) ------------------------------------------------------------
FROM ${UBUNTU} AS web-build
ARG EMSDK_VERSION
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates cmake ninja-build git python3 xz-utils \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git /emsdk \
 && /emsdk/emsdk install "${EMSDK_VERSION}" \
 && /emsdk/emsdk activate "${EMSDK_VERSION}"
WORKDIR /src
COPY . .
RUN . /emsdk/emsdk_env.sh \
 && cmake --preset wasm \
 && cmake --build --preset wasm --target px_player \
 && mkdir -p /out \
 && cp build/wasm/apps/player/px-player.js build/wasm/apps/player/px-player.wasm build/wasm/apps/player/px-player.data /out/

FROM platform-base AS platform-web
COPY --from=web-build /out/ /opt/pixel/web-player/

# --- The default: no browser player ---------------------------------------------------------------
FROM platform-base AS platform
