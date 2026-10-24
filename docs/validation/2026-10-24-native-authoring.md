# Matched native Studio authoring checks, 2026-10-24

Nine native acceptance selectors passed on merged Studio and the provenance-matched
macOS SDK below. The result closes the missing-native-SDK execution prerequisite
recorded in the [earlier authoring-loop evidence](2026-10-24-authoring-loop.md)
for this exact composition. No Studio, engine or SDK source change was needed.

## Exact composition

| Input | Identity |
| --- | --- |
| Merged Studio | `824d3112f6f7a4da6bd785d015f106fd11b41cca` |
| Studio tree | `2f93a663f10b7925239a0d92483fb7024eaaec42` |
| Engine source | `845367848547b9d04612744fa70c65545717525d` |
| Native SDK package ID | `5ecad31eb406b3f49e5f283f6a010b497b4ad799711beb5d3536297f0bf21655` |
| SDK package.json SHA-256 | `eab2353498dc488d6a0104705f71fdbfd00d762474444e7439e185eff34c7bb5` |
| Platform | Darwin arm64 |
| Compiler | Clang 22.1.8 |
| Configuration | Release |
| Maximum build jobs | 2 |
| chibi-dev SHA-256 | `c5578ff6c16019938e9640b424b173651a9a97f7eee00a7ed51cfa56419d42ae` |
| studio-acceptance SHA-256 | `4f832be92c4ae4e967a36e41f091b1d9e0e27b659aa8588d8cfef2b2512001c9` |

The native export worker supplied an installed SDK and its handoff receipt.
Receipt and manifest agreed on the clean engine revision, package ID, platform
and toolchain. All 1,252 manifest files passed independent byte-count and SHA-256
verification before configuration and again after the acceptance runs. The SDK
had no extra unlisted files afterward. It was not rebuilt, installed into,
patched or otherwise modified.

The task's isolated worktree switched to a new branch from merged Studio
`824d3112f6f7a4da6bd785d015f106fd11b41cca`. The delivered chooser-fix branch was
retained. This evidence commit adds only documentation and test receipts.

## Build and execution

With `SDK_PREFIX` set to the received read-only SDK, and `ZEP_SOURCE` set to a
local copy of the existing pinned and patched Zep dependency, the build used:

```sh
CC=/opt/homebrew/opt/llvm@22/bin/clang \
CXX=/opt/homebrew/opt/llvm@22/bin/clang++ \
cmake -S chibi-studio -B build-native845 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DCHIBI_STUDIO_DISTRIBUTION=ON \
  -DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON \
  -DFETCHCONTENT_SOURCE_DIR_ZEP="$ZEP_SOURCE"
cmake --build build-native845 --target chibi-studio studio-acceptance --parallel 2
```

Both targets compiled and linked. CMake recorded the package ID and manifest
checksum above. The linker reported duplicate static-library warnings; no
compile or link error occurred. Configure/build time is excluded from the
execution times below.

Every selector ran through the existing `tools/test_policy.py` runner, using
its `native` action and descendant cleanup. The initial sandboxed attempt stopped
before starting any test because process inspection through `ps` was unavailable.
The same runner then ran with the required host permission. Cleanup was not
bypassed. The runner came from the existing engine checkout at
`d0de4a84ecf50badcab661411470bdf6f3da5fa2`, SHA-256
`8e51a66ee7b52f072cabebf91a1381b06f18c9cbfd3aca9b543fd02ced329f07`.
Comparison with engine845 found only named test inventory and group-list changes;
the direct native execution and cleanup implementation used here is identical.
CPU invocations used a 60-second runner limit; the three bounded host
invocations used 120 seconds. All finished normally below those limits.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 "$ENGINE_SOURCE/tools/test_policy.py" \
  --timeout 60 native "$PWD/build-native845/bin/studio-acceptance" --sdk-values
```

The source-edit, prototype-source and prototype-source-authority selectors each
received a separate APFS clone of `tests/acceptance/project`. Before each host
invocation, `projects/studio/project.kdl` was copied back into the build's
`share/chibi/studio` resource tree as the acceptance instructions require. Only
these disposable/build copies could migrate. Preferences and thumbnail caches
were isolated inside the disposable fixture. `SDL_AUDIODRIVER=dummy` was set.
The passed fixture copies were removed after validation.

The Outliner policy selector used the checked-in
`projects/studio/content/libraries/OutlinerRowPolicies.chibi` source. It executed
41 direct calls on bytecode and 41 on actual SLJIT, with backend-entry assertions.

## Results

| Selector | Passed pico cases | Assertions | Hidden host frames | Runner wall time, seconds |
| --- | ---: | ---: | ---: | ---: |
| `--project-location` | 5 | 32 | 0 | 0.812 |
| `--sdk-values` | 12 | 163 | 0 | 0.238 |
| `--scene-cursors` | 1 | 29 | 0 | 0.092 |
| `--scene-read` | 6 | 1,141 | 0 | 0.093 |
| `--project-open` | 1 | 50 | 0 | 0.093 |
| `--outliner-row-policies` | 1 | 1,940 | 0 | 0.100 |
| `--source-edit` | 1 | 8,935 | 4 | 2.390 |
| `--prototype-source` | 1 | 270 | 2 | 1.175 |
| `--prototype-source-authority` | 1 | 881 | 2 | 0.919 |
| Total | 29 | 13,441 | 8 | |

The [machine-readable receipt](2026-10-24-native-authoring/results.json) contains
the same counts, timings and identities.

The value selector checks owned string bytes, native/descriptor text,
checkpoint restoration and real ECS Undo/Redo, complete-batch preflight,
collaboration graph payloads and optional Studio catalog actions. The scene-read
selector checks six private owned-capture contracts with thirteen captures.
Project-open checks invalid inputs before UI initialization. These first six
selectors use no renderer.

The source-edit selector runs four hidden frames with one production graph
expression edit, Undo and Redo. Its cumulative existing fixture also checks
static prototype/relationship source models, save/history/checkpoint ownership,
registry refresh, discard and source-document dispatch. Counter's original bytes
remain unchanged at both ends.

Prototype-source runs two hidden frames with four prototypes, three actors and
one checkpoint. It reports `deleted_source_retained=1` and
`queued_commands_quiet=1`. Prototype-source-authority runs two hidden frames
with one prototype, no actors, one checkpoint, 68 incoming and 96 outgoing
messages. It reports Code and Properties unchanged before acceptance,
`retry_held=1`, accepted held/live intent and retained disk conflict. These are
source/model and authority checks on the merged implementation, not the
unmerged typed-asset handoff.

Logs contain no `Studio UI:`, `runtime frame failed`, `Could not initialize`,
`GLSL link error` or failing pico marker. No CUA interaction or lock-screen
change was attempted. The headless test processes exited through the runner.

## Coverage limits

This is a separate native pair, Studio `824d3112` with engine `8453678`, from the
earlier browser interaction pair pinned to engine `0f45ed129`. Results are not
transferred between them. No new browser build or interaction was run here.

These finite native checks do not establish manual project creation,
input-driven gameplay, native app close/reopen, all controls, cross-platform
behavior, export or general game readiness. No independent screenshot/visual
approval is claimed for the locked Mac. The existing browser HRC sampler-limit
finding remains a separate renderer concern; no HRC source was edited or
WebGL lighting acceptance rerun. This Release build is not a sanitizer or leak
check. No broad engine suites or full Studio acceptance suite ran, and no
release or deployment action occurred.
