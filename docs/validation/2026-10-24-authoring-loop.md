# Studio authoring loop, 2026-10-24

The bounded 2D browser loop passed on the pair below. A separate native New
Project folder-selection fix has a reproduced failure and passing C regression
cases. Neither result establishes general game readiness or native UI acceptance.

## Revisions and build

| Input | Identity |
| --- | --- |
| Studio baseline | `df050886dd7721db63b40fdf5435f1cb20ceeb34` |
| Studio source candidate | `8b315feacdc307acf54868692408d9cc63365f14` |
| Engine | `0f45ed129742965122f7474c53301a1f829f1743` |
| Published web SDK build ID | `7f838063479af735342156257e5561afcac9a9f813c7f8b66f54bcd3bf212f57` |
| SDK archive SHA-256 | `3921373a6cdb9612f805fda3ce0f414a48d33578984244286fa071a61e24391c` |
| SDK package.json SHA-256 | `282ff05061d835def710988123811d458c75140afcc5c955c1291d7cae1c4646` |
| Zep, with Studio's existing patches | `7aa7ed940d5c889dc6b27a2529a740c0bb2ad6af` |

The SDK came from the engine's published `engine-0f45ed129742965122f7474c53301a1f829f1743`
release. Its source manifest identifies that clean engine revision. The archive
checksum matched the release's `SHA256SUMS`. Neither typed-editor handoff nor
Studio77 recovery source was combined into this candidate.

The build used the already installed Emscripten `6.0.9-git`. The SDK release's
`build.json` records Emscripten `6.0.10`. That toolchain difference remains a
limit of the local result. Both initial and candidate web builds completed with
two jobs. A task-local Emscripten cache avoided writes to Homebrew's shared cache.

From the task directory, the build commands were:

```sh
emcmake cmake -S chibi-studio -B build-web -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DChibi_DIR="$PWD/web-sdk/lib/cmake/Chibi" \
  -DFETCHCONTENT_SOURCE_DIR_ZEP="$PWD/zep-src"
EM_CACHE="$PWD/emscripten-cache" \
  cmake --build build-web --target chibi-studio --parallel 2
python3 chibi-studio/tools/serve_web.py --directory build-web/bin --port 8764
```

The candidate artifacts were:

| Artifact | SHA-256 |
| --- | --- |
| chibi-studio.wasm | `923aacc46edb9e1db60f8e094bf7de0e54cfd28899857b917c6cb6c9ea050d48` |
| chibi-studio.js | `27a03455bdd42a0401fe0342717ff29e3289c97dcbfcd8d243b128cf69401e7d` |
| chibi-studio.data | `2fc2643f59bdbcfa16a5cbff20ec36ad7b0c46aaf5e0152658f2f2274f1be3b8` |
| chibi-studio.html | `8cbae2b7b714f92cc333a9d56f12a93cfe100741415826d818c0fee2d2987b60` |

The later documentation commit does not change these compiled inputs. Newer
engine main was reported during the task; it was not substituted into this pair.

## Manual browser evidence

The Codex in-app browser opened the local build at `http://127.0.0.1:8764/`.
This was a new origin for the disposable project. Existing projects and user
tabs were not edited. Screenshots in the implementation thread record the
created project, saved source, running scene, live Transform and final reopen.

On the baseline, File > New > Project created `Authoring Loop 20261024` and its
Start scene. File > New > Prototype created `LoopPawn`. The Outliner + picker
inserted an instance; F2 renamed it `SavedPawn`. Add Component supplied Transform
and 2D Appearance. The numeric editor set X to 24, and Save wrote that value.
Closing the browser tab and opening the same project again preserved the name,
components and X = 24.

New Behavior created `LoopTick`. Its source was edited and saved through Code:

```chibi
LoopTick :: behavior {
    on Tick :: () {
        self.Transform.x = self.Transform.x + 1
    }
}
```

The entity's Behaviors inspector attached LoopTick, and Save wrote the attachment
to Start.kdl. Play rendered the white box and the live Transform advanced from
24 to 335 in an observed frame. Stop restored the authored X = 24. The graph's
Tick selector showed the matching handler and setter nodes. The empty Variables
graph initially mistaken for stale output was not a defect.

After loading the rebuilt candidate, the same project retained SavedPawn,
X = 24, both components and the LoopTick attachment. A fresh field edit set
authored X to 36. Play advanced live X to 49 in the captured frame. Stop restored
X = 36. Save completed, the browser's saving status cleared, and the tab closed.
Opening the project in a new tab preserved SavedPawn, X = 36, the white box,
components and LoopTick attachment. The window title was clean.

This covers one small 2D scene, one source edit and one Tick handler. It does
not cover input-driven gameplay, collaboration, 3D, export, native dialogs,
cross-browser behavior, recovery after process termination or all authoring
controls. The project remains disposable browser storage on this local origin.

## Native folder-selection regression

The original native chooser used `stat`, which follows links. A dangling
`my-project` symlink made it return an occupied path, so project creation would
fail rather than try `my-project-2`. A looping symlink stopped selection with
ELOOP. The extracted chooser preserves the original slug and suffix algorithm
and uses `lstat` to inspect the directory entry instead.

The new five pico cases run through `studio-acceptance --project-location`, or
the SDK-free C driver documented in [the acceptance inventory](../../tests/acceptance/README.md).
The latter compiled the actual production helper with
`clang -std=c23 -Wall -Wextra -Werror -Wl,-dead_strip` and ran these same cases.

| Source | Result |
| --- | --- |
| Extracted original `stat` implementation | 3 passed, 2 failed, 28 assertions |
| Candidate `lstat` implementation | 5 passed, 0 failed, 32 assertions |

The two failures were the dangling-link selected-path assertion and the
looping-link success assertion. Positive checks cover unused paths, directories,
regular files, valid symlinks, preserved sentinel bytes, creating the chosen
directory, preserving the dangling link, path truncation and an invalid parent.
Temporary test directories were removed after each case. No renderer, SDK,
project compiler or GPU was needed for these C executions.

The native `studio_projects.c` call site also passed Clang syntax checking with
the pinned public SDK headers and the local web build's SDL 3.4.2 headers. This
was not a native link or native UI test. clang-format 22 checks on the new C files,
changed-line formatting, `git diff --check`, and the edited extracted file's
consumer hash passed. Source review covered the copied slug algorithm, caller
buffer lifetime, errno propagation, collision handling and fixture cleanup.

## Remaining limits

- The accepted engine release had Linux and web SDK assets, but no macOS SDK.
  Existing local native SDK manifests identified other or dirty revisions, so
  none was used to claim native acceptance of this pair.
- Syntax-checking the complete existing acceptance driver against this web SDK
  stops at `tests/acceptance/outliner_row_policies.h:9`, which includes
  `runtime/arrays.h`; that header is absent from the published SDK. The new
  folder-selection cases ran independently. The full acceptance executable and
  the originally intended `--sdk-values` selector remain unrun.
- The browser logged `GLSL link error` and
  `FRAGMENT shader texture image units count exceeds MAX_TEXTURE_IMAGE_UNITS(16)`
  on the pinned pair. The observed 2D render and Tick execution still passed.
  Shader changes belong to the engine owner; this record makes no claim about
  the subsequently reported main revision.
- Native/material/project/language/tool engine suites were not run for this
  Studio-only change. No engine source, compiler, bootstrap, typed-asset runtime,
  source reader or project migration was edited. No deployment occurred.
