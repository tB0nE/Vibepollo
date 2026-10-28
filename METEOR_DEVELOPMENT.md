# Meteor-Vibepollo for Nightfall — development baseline

Created 2026-10-28 from the user's existing Vibepollo development checkout.
This independent checkout is the source baseline for the host managed by
Nightfall Meteor. Existing Sunshine-compatible external hosts remain supported.

## Repository and source

- Local checkout: `~/Development/Personal/meteor-vibepollo`.
- Origin (user fork): https://github.com/tB0nE/Vibepollo.git
- Upstream: https://github.com/Nonary/Vibepollo.git
- Development branch: `codex/meteor-vibepollo-baseline`.
- Source baseline: `4ae9e50c3af5dd56cf2830f0a5f2d3170f30a90b`, originally
  `linux-x11-nvfbc-pyrowave`. Includes GPU-resident NvFBC capture work.
- The original checkout had no tracked modifications. Only `build-cuda/` and
  `build-x11/` were untracked; neither is part of this source baseline.
- The clone has its own Git objects, not a shared worktree or hardlinked clone.
  Its source history was shallow at creation, matching the source checkout;
  upstream history can be fetched later when needed.

## Existing executable is a separate checkpoint

The existing `Vibepollo/build-cuda/vibepollo-2.0.0` executable reports source
revision `8a8c4b03a280ab9f567beb380110abb80f5220b8`, which precedes the source
baseline above. Do not claim a newly compiled 4ae9e50 executable is already the
same tested binary.

Existing executable SHA-256:
`0776a1c48448afd3061aca2083cd7d0240f187d63c312f215f36316aefe6b031`.

The existing executable and build directories remain in the original checkout
for comparison. No host binary, account state, pairing keys, credentials or
user configuration is committed here. This source checkpoint has not yet been
rebuilt or deployed. Branding within the application is also still upstream;
this document establishes the development baseline, not a finished product fork.

## Initialize and build

Git submodules retain their exact pinned commits. This initial source checkout
leaves them uninitialized; before configuring a fresh build:

```sh
git submodule update --init --recursive
```

Read the applicable repository instructions before modifying a directory,
including `docs/linux/AGENTS.md` for Linux documentation work. Use release builds
for performance testing. Do not copy a CMake build tree to this directory:
its cache and generated commands refer to the original source/build paths.
Configure a new build directory instead.

The original CMake cache records these relevant options (a baseline inventory,
not a complete reproducible command):

- `CMAKE_BUILD_TYPE=Release`
- CUDA, X11, NVFBC_VK, VAAPI, PyroWave and tray enabled.
- DRM, Wayland, Portal, KWin, Gamescope, Vulkan option and WebRTC disabled.
- CUDA architecture setting was 75; do not assume this is the target GPU for
  a distributable package or conflate it with the detected RTX3090 hardware.

That development executable also needs locally supplied compatibility libraries
(ICU/miniupnpc). The distributable Meteor host must not depend on those personal
paths. Consult Nightfall's `docs/plans/active/meteor-bundled-host.md` for the
self-contained Ubuntu 22.04 packaging goal, runtime-library isolation and capture
backend requirements.

## Current test failure and next work

The PC uses an AMD RX480 for its X11 desktop and an NVIDIA RTX3090 for offload.
The previously working external host explicitly selects:

```ini
capture = x11
encoder = nvenc_legacy
```

The freshly managed host initially selected native NVENC instead. Encoder-only
synthetic probing passed, but actual stream startup failed three attempts at
native-device creation. Thus a successful encoder probe is not proof that a
capture source can feed it, particularly across GPU vendors. Meteor's managed
configuration was changed locally to the two explicit compatibility settings
above for the next test; no successful stream after that change is claimed here.
Do not make them universal defaults or silently fall back on every machine.

The running managed host still references the original development executable.
Changing that path to a fresh build from this checkout is a separate deliberate
step after build validation. The original host checkout/configuration and monitor
layout must remain intact. Do not add file capabilities or change firewall or
network configuration without explaining the change first.

Next milestones:

1. Rebuild this exact source baseline in a new release build directory and
   compare it with the existing executable, including mixed-GPU capture/encode.
2. Establish a working Meteor-managed stream with correct pairing and client
   routing; preserve separate host identities and settings.
3. Build a relocatable host package with relative assets and required libraries,
   suitable capture backends and license/source material. Publish a pinned
   archive before enabling Meteor's download/install offer.
4. Complete host-switch communication, pairing approval and accessible host
   administration in Meteor. Its generated web login currently has no usable
   retrieval flow in the tray.
5. Validate discovery, input/audio, restart/reconnect, external-host coexistence
   and clean-machine setup. Windows and environment-design work are deferred.
