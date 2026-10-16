# Bounded offscreen Studio rendering on Linux

Studio already supports finite rendering and framebuffer readback through
`--headless --frames N --fixed-dt SECONDS --screenshot PATH`. `--headless` hides
the SDL window but keeps graphics enabled. Unlike bounded `chibi_ctl --render`
and `--render-request`, Studio does not automatically select SDL's offscreen
video driver. Set `SDL_VIDEO_DRIVER=offscreen` explicitly.

## Prerequisites

- An SDL build containing the offscreen Vulkan driver. The validated historical
  SDK uses SDL 3.4.0. Its implementation creates a Vulkan headless surface.
- A working Vulkan loader and driver supporting `VK_EXT_headless_surface`, such
  as the verified Mesa llvmpipe software stack. Driver enumeration alone does
  not prove a rendered frame. Software rendering uses CPU and memory; do not
  report its timing as hardware GPU performance.
- The matching installed SDK resources beside the Studio executable. Keep
  `bin/chibi-dev` and `share/chibi` together; Studio discovers its UI and authored
  Studio project relative to the executable. An engine-resource override alone
  does not relocate those Studio resources.
- A writable task-local project, preferences, runtime directory and caches.
  Do not open a user's working project for a startup probe. Studio may update
  copied project/resource files while opening them.

No display server or X/Wayland listener is needed by this route. Installing or
selecting a driver remains an explicit environment setup step. Do not change
security settings, use a different transport to evade a denied listener, or
infer support from a host name. If initialization fails, preserve the exact
SDL/Vulkan error rather than falling back automatically.

## Isolated two-frame capture

Run from a matching engine checkout, using an already built Studio and an
existing approved renderer environment. Set `STUDIO_BUILD`, `STUDIO_SOURCE`
and `CAPTURE` to absolute paths. The capture destination must not already exist.
For task-local Mesa, preserve the verified `LD_LIBRARY_PATH` and
`VK_DRIVER_FILES` from setup; do not guess a different driver path.

```sh
set -eu
test ! -e "$CAPTURE"
scratch=$(mktemp -d)
mkdir -p "$scratch/runtime/bin" "$scratch/runtime/share" \
  "$scratch/prefs" "$scratch/cache" "$scratch/tmp" "$scratch/xdg"
cp --reflink=auto "$STUDIO_BUILD/bin/chibi-dev" "$scratch/runtime/bin/"
cp -a --reflink=auto "$STUDIO_BUILD/share/chibi" "$scratch/runtime/share/"
cp -a --reflink=auto "$STUDIO_SOURCE/tests/acceptance/project" "$scratch/project"
unset DISPLAY WAYLAND_DISPLAY SDL_VIDEODRIVER
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIODRIVER=dummy
export CHIBI_STUDIO_PREFS_PATH="$scratch/prefs/studio.json"
export CHIBI_PREFS_DIR="$scratch/prefs"
export CHIBI_RESOURCE_DIR="$scratch/runtime/share/chibi"
export XDG_CACHE_HOME="$scratch/cache" MESA_SHADER_CACHE_DIR="$scratch/cache"
export XDG_RUNTIME_DIR="$scratch/xdg" TMPDIR="$scratch/tmp"
python3 - "$scratch" "$CAPTURE" <<'PY'
import sys
sys.path.insert(0, "tools")
import test_policy
scratch, capture = sys.argv[1:]
sys.exit(test_policy.run_commands([[
    scratch + "/runtime/bin/chibi-dev",
    "--project-root", scratch + "/project",
    "--headless", "--frames", "2",
    "--fixed-dt", "0.016666666666666666",
    "--screenshot", capture,
]], seconds=60))
PY
```

This uses the unchanged production process-group/descendant supervisor. Its
whole-operation deadline includes initialization, both frames, rendering,
readback and teardown. The existing screenshot helper has an additional
10-second readback wait inside that outer deadline. No checkpoint is supplied,
so Studio's window/capture canvas is 1920×1080. External preparation/build time is separate.

Require exit 0 and a freshly created, decodable 1920×1080 PNG. Inspect the pixels
and actual Studio panels/text; a file or successful device enumeration alone is
not sufficient. Record the executable/source/SDK/resource/driver identities,
stdout/stderr, time, image hash and dimensions, and any initialization or runtime
errors. Confirm no child remains. Preserve evidence before deleting only the
scratch project/runtime/preferences/cache. On failure retain scratch for
inspection and stop; do not launch an unbounded Studio session.

Two frames prove bounded startup and framebuffer readback. They do not exercise
Outliner input, browser behavior, visual parity, lifecycle transitions or the
full acceptance suite. Each meaningful interaction workload needs its own
source-reviewed finite admission and the same complete-command deadline.

## Validation record

A single source-reviewed invocation on 2026-10-16 exited 0 in 6.948 seconds
under the unchanged 60-second supervisor. Its 1920×1080 PNG was decoded and
visually inspected: Studio chrome, the viewport grid, empty Outliner, Properties
and behavior panels contain readable text. The isolated fixture has an empty
Start scene; this observation is a startup/readback result, not input coverage.

- Studio source: `78cb4d4c47242db9f7af2b9573653d9756050e99`.
- Installed native SDK engine: `c159b6cd67d1623f3a7d5cc3a434b04fb849e808`.
- `chibi-dev` SHA-256:
  `96b6204da737f1f4b89bea367c050bb3a99af7ed1f1670f86391ee31a3acc16e`.
- Screenshot SHA-256:
  `378a65dbe62e7e6c28601ba905f6f4c00defed0fdc0bab3f650ca1ee50135792`.
- Environment: SDL offscreen, official Debian Mesa 25.0.7 llvmpipe, LLVM 19.1.7;
  `VK_DRIVER_FILES` pinned the verified software ICD. Prior capability discovery
  reported that device and headless-surface support. The capture log did not
  independently print the device name.

The historical SDK and all source/build/resource/driver identities were checked
before execution. This does not establish current-engine SDK compatibility or
native/web parity. No engine or Studio product change was needed to render.
