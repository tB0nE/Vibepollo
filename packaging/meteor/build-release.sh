#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-meteor-release"
ENGINE="${CONTAINER_ENGINE:-docker}"
"$ENGINE" build -t nightfall-meteor-host:ubuntu22.04 -f "$ROOT/packaging/meteor/Dockerfile.builder" "$ROOT/packaging/meteor"
mkdir -p "$BUILD"
FFMPEG_INPUT="$ROOT/.build-cache/ffmpeg-v2026.516.30821"
mkdir -p "$FFMPEG_INPUT"
if [[ ! -f "$FFMPEG_INPUT/Linux-x86_64-ffmpeg.tar.gz" ]]; then
 curl -fL --retry 3 https://github.com/LizardByte/build-deps/releases/download/v2026.516.30821/Linux-x86_64-ffmpeg.tar.gz -o "$FFMPEG_INPUT/Linux-x86_64-ffmpeg.tar.gz"
fi
printf '%s  %s\n' c32319fcc2867befe8ff3ae26b4d3a58378c3a9c1bfef9029c9969321c3bc6ec "$FFMPEG_INPUT/Linux-x86_64-ffmpeg.tar.gz" | sha256sum --check
if [[ ! -f "$FFMPEG_INPUT/ffmpeg/lib/libavcodec.a" ]]; then
 tar -xzf "$FFMPEG_INPUT/Linux-x86_64-ffmpeg.tar.gz" -C "$FFMPEG_INPUT"
fi
"$ENGINE" run --rm --user "$(id -u):$(id -g)" \
 -v "$ROOT:/src:rw" -w /src -e BUILD_VERSION -e COMMIT -e npm_config_cache=/src/.build-cache/npm \
 nightfall-meteor-host:ubuntu22.04 bash -euo pipefail -c '
extra=()
[[ ! -d /src/build-inputs/boost ]] || extra+=(-DFETCHCONTENT_SOURCE_DIR_BOOST=/src/build-inputs/boost)
[[ ! -d /src/build-inputs/json ]] || extra+=(-DFETCHCONTENT_SOURCE_DIR_JSON=/src/build-inputs/json)
cmake -S /src -B /src/build-meteor-release -G Ninja \
 -DCMAKE_BUILD_TYPE=Release -DSUNSHINE_BUILD_METEOR=ON \
 -DFFMPEG_PREPARED_BINARIES=/src/.build-cache/ffmpeg-v2026.516.30821/ffmpeg \
 -DSUNSHINE_REPO_OWNER=tB0nE -DSUNSHINE_REPO_NAME=Vibepollo \
 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-14 \
 -DSUNSHINE_REQUIRE_CUDA_PASCAL=ON -DSUNSHINE_ENABLE_CUDA=ON \
 -DSUNSHINE_ENABLE_X11=ON -DSUNSHINE_ENABLE_WAYLAND=ON \
 -DSUNSHINE_ENABLE_KWIN=ON -DSUNSHINE_ENABLE_PORTAL=ON \
 -DSUNSHINE_ENABLE_DRM=OFF -DSUNSHINE_ENABLE_VULKAN=OFF \
 -DSUNSHINE_ENABLE_VAAPI=ON -DSUNSHINE_ENABLE_PYROWAVE=ON \
 -DSUNSHINE_ENABLE_NVFBC_VK=ON -DSUNSHINE_ENABLE_TRAY=OFF \
 -DSUNSHINE_ENABLE_WEBRTC=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF \
 -DCMAKE_BUILD_RPATH_USE_ORIGIN=ON \
 -DSUNSHINE_PUBLISHER_NAME=Nightfall \
 -DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/tB0nE/Vibepollo \
 -DSUNSHINE_PUBLISHER_ISSUE_URL=https://github.com/tB0nE/nightfall/issues "${extra[@]}"
cmake --build /src/build-meteor-release --target sunshine --parallel "${BUILD_JOBS:-6}"
'
