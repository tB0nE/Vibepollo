# Portable Meteor-Vibepollo host (Linux preview)

This is an opt-in user-local package for Nightfall Meteor. It does not install
system services, kernel modules, capabilities, display layouts or firewall rules.
The existing machine-service and upstream build profiles keep their behavior.

From a Git checkout, initialize pinned submodules, then build:

```sh
git submodule update --init --recursive
bash packaging/meteor/build-release.sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:rw" -w /src \
  nightfall-meteor-host:ubuntu22.04 python3 packaging/meteor/package-release.py
python3 packaging/meteor/package-sources.py
```

The isolated toolchain uses Ubuntu 22.04, GCC 14 and CUDA 12.9 (retaining the
Pascal-compatible release policy). Nothing is installed into the build machine's
system. The FFmpeg binary input is pinned by version and SHA-256; its exact source
and codec submodules are included in the source companion. Boost and JSON inputs
are pinned by the existing CMake hashes. Web interfaces are built from the lockfiles.
Docker base tags and Ubuntu package updates can change over time: package metadata
records the dependency versions; this is not a byte-for-byte reproducible build claim.

Outputs are under `.build-cache/meteor-host-release/`. Archive version
`0.1.0-preview.1` identifies the Meteor packaging release; the inherited host's
internal version remains Vibepollo 2.0.0 with the source commit in `--version`.
The archive contains `bin/meteor-vibepollo`, `bin/vibepollo`, `lib/`,
`share/vibepollo/`, notices and a per-file manifest. Meteor verifies its pinned
archive digest and installed files before switching to the host.

The executable resolves its package-relative assets from `/proc/self/exe`,
including when run from another directory. Private libraries use relative
RUNPATHs, not a global LD_LIBRARY_PATH that would leak into launched games.
Host graphics, device, desktop and audio libraries stay with the installed OS,
so driver and PipeWire/GIO plugin versions match. The package inventories those
requirements and checks that its own ELFs need no newer than glibc 2.35.

Enabled: X11, CUDA/NVENC, VA-API, PyroWave/NvFBC, Wayland, KWin, portal and
Gamescope capture. The separate Vulkan video encoder, host tray and privileged
KMS/machine-service deployment are excluded from this first portable package.
Supported compilation does not establish hardware compatibility: acceptance
still needs real desktop streams, controller/input/audio, restart/reconnect and
Wayland/NVIDIA/AMD user testing. Mixed-GPU capture and encoding require particular
care; retain the original working executable for comparison.

Before release publication, run the relocation and isolated-credential smoke
checks, inspect the file manifest, prepare a matching source companion, then
upload both archives plus checksums to a host preview release in
`tB0nE/Vibepollo`. Pin the actual public URL/size/hash in Nightfall Meteor's
`src/host/install.rs`. Do not upload development credentials or user profiles.

Building an extracted source companion does not require Git for the host itself;
set `BUILD_VERSION=2.0.0` and `COMMIT` to its recorded source revision when running
the build script. The companion includes the initialized Linux submodules, plus
Boost and JSON sources under `build-inputs/` and their inventories. Optional
uninitialized platform submodules are listed explicitly rather than silently
presented as included. Network access is still needed for the container/toolchain
and pinned FFmpeg binary input; its build scripts and corresponding codec source
are included for rebuilding that dependency separately.
