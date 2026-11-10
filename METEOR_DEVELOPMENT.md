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

## Current multi-monitor integration checkpoint (2026-10-28)

The portable host packaging and capture-aware automatic encoder selection are
implemented. Published preview.2 uses source `382bf6a`. The old failure notes and
original milestone list below are historical, not the current work queue.

Restored the later work from the original checkout's `nightfall` branch:

- `3ae53454df8254c8b4903d6ddb2bedc8bce5a969` → `6a574f0`:
  X11 multi-monitor capture and the Polaris-compatible display manifest API.
- `0f7878565f2357f17f88c9be688ff2b69d0ccc9e` → `ea94a2b`:
  full host permissions for each newly approved paired client. Existing disabled
  clients retain the upstream behavior. This is a host authorization policy;
  it does not grant Linux device access or replace Meteor's uinput setup.

The imports applied cleanly and retain their original authorship plus
cherry-pick provenance. Capture-aware NVENC selection remains present.
Twenty-seven focused tests pass: 14 monitor geometry/packing/input-mapping
cases and 13 NVENC API/selection cases. They do not access live displays or GPUs.

The monitor feature selects outputs through `outputs=` on launch/resume and
reports their positions through `/polaris/v1/display/manifest`. Selected monitors
share one encoded frame; Nightfall splits that frame into screen views. Oversized
layouts can be packed into rows and pointer coordinates map back to the desktop.
It does not create virtual monitors, offer independent per-monitor streams, or
implement Wayland multi-monitor capture. Real multi-monitor streaming is still
an acceptance item before this is promoted into Meteor's pinned host download.

Build using `packaging/meteor/build-release.sh` (Release, Ubuntu 22.04 container).
Stage development packages with `METEOR_HOST_VERSION=0.1.0-preview.3-dev` passed
into the package-release container. Do not overwrite preview.2 artifacts or claim
the published release contains these imports. Keep real desktop layouts intact
while testing; synthetic geometry tests cover layouts without changing xrandr.

## Existing executable is a separate checkpoint

The existing `Vibepollo/build-cuda/vibepollo-2.0.0` executable reports source
revision `8a8c4b03a280ab9f567beb380110abb80f5220b8`, which precedes the source
baseline above. Do not claim a newly compiled 4ae9e50 executable is already the
same tested binary.

Existing executable SHA-256:
`0776a1c48448afd3061aca2083cd7d0240f187d63c312f215f36316aefe6b031`.

The existing executable and build directories remain in the original checkout
for comparison. No host binary, account state, pairing keys, credentials or
user configuration is committed here. The initial baseline was not rebuilt or deployed. Subsequent portable-release
work is described in `packaging/meteor/README.md`. Branding within the application is still upstream;
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

## Historical initial test failure and original milestones

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

At the initial handoff, the managed host still referenced the original development executable.
Changing that path to a fresh build from this checkout is a separate deliberate
step after build validation. The original host checkout/configuration and monitor
layout must remain intact. Do not add file capabilities or change firewall or
network configuration without explaining the change first.

Original milestones (subsequently completed or superseded as described above):

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

## Wayland multi-monitor reuse (2026-10-28)

Polaris source `7f6c2b6` contains KDE Full workspace capture, subset cropping,
logical pointer calibration and reconnect fixes (notably `da37cde`). Its old
Wayland plan predates those changes. Do not import the old N-source wait loop.

The KDE output parser and scale metadata are now ported as a foundation, with
per-output type validation and eight parser tests. Together with existing
geometry and NVENC policy coverage, 35 focused tests pass. The parser is built
but not yet called by the capture path; this does not enable Wayland multi-monitor.

Next: shared negotiated-buffer geometry and pointer mapping, then adapt the
existing portal/PipeWire capture path, manifest and reconnect lifecycle together.
The full plan is in Nightfall `docs/plans/active/meteor-wayland-multi-monitor.md`.
No EVDI changes, display reconfiguration, deployment or host release were made.

### Native KWin workspace implementation

The initial Wayland capture implementation now uses KWin `stream_region`
(version >= 3) at scale 1.0 through the existing PipeWire/encoder backend.
It follows Polaris's one-workspace strategy without copying its capture
engine or adding per-frame CPU cropping. XDG output supplies logical monitor
geometry directly, so kscreen-doctor is not a runtime dependency.

Shared `wayland_workspace.h` geometry drives capture, manifest and input.
Automatic KDE capture prefers KWin before portal probing. Explicit selections
are checked before launch/resume; unknown names, unsupported backends,
non-contiguous crops and oversized layouts fail rather than capture a fallback.
The manifest preserves `host_manifest`; negotiated size must match exactly.
Private-display physical capture remains on its existing path.

Validation: Release build passes; 44 focused tests pass (including nine new
workspace cases). In a temporary headless KWin session with two virtual outputs,
the host enumerated both and negotiated its single-output workspace region
at 640x360 using PipeWire memory buffers. This is initialization coverage,
not acceptance of a two-monitor Nightfall stream or GPU performance.
Reports: `.build-cache/meteor-host-release/wayland-smoke/summary.json` and
`test-build/wayland-workspace-results.json`.

No live desktop reconfiguration, deployment or publication was performed.
Outstanding: real KDE client stream/input/reconnect testing, oversized GPU
packing, and non-KDE portal multi-output support. Virtual-output creation is
outside this port. See the Nightfall plan for the acceptance checklist.
