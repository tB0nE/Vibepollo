FROM node:22-bookworm-slim AS node
FROM nvidia/cuda:12.9.1-devel-ubuntu22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common ca-certificates gnupg \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends \
 gcc-14 g++-14 ninja-build git python3-pip pkg-config curl patchelf zstd \
 libcap-dev libcurl4-openssl-dev libdrm-dev libevdev-dev libgbm-dev libminiupnpc-dev \
 libnuma-dev libopus-dev libpipewire-0.3-dev libpulse-dev libssl-dev libsystemd-dev \
 libudev-dev libwayland-dev libx11-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb1-dev \
 libxfixes-dev libxrandr-dev libxtst-dev libvulkan-dev libva-dev libsqlite3-dev \
 libpng-dev libjpeg-dev libwebp-dev libglib2.0-dev libgl-dev libegl-dev \
 glslang-tools wayland-protocols gettext \
 && python3 -m pip install --no-cache-dir cmake==3.30.5 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules/npm /usr/local/lib/node_modules/npm
RUN ln -s ../lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s ../lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx
ENV CC=gcc-14 CXX=g++-14 CUDAHOSTCXX=g++-14
RUN python3 -m pip install --no-cache-dir jinja2==3.1.6
