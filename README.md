# Chibi Studio

This repository owns Studio's editor, document and workspace code, its application
loop, retained UI kit, text editor bridge, desktop host, and authored Studio UI.
It consumes an installed Chibi SDK. Engine, compiler, renderer, shared UI backend
and native runtime implementations remain in their owning SDK packages.

`EXTRACTION.json` records every copied source's original path and SHA-256 hash.
Preserved test oracles live in `tests/held/engine_oracles`; they are held coverage,
not passing tests. Agent and contributor rules are in `AGENTS.md`.
The engine's [current status](https://github.com/waldnercharles/chibi/blob/main/docs/status.md)
tracks implemented capabilities, remaining work, ownership and evidence limits
across repositories.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIBI_STUDIO_DISTRIBUTION=ON \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/pinned/chibi-sdk
cmake --build build
python3 /absolute/path/to/pinned/chibi-sdk/share/chibi/tools/package_studio.py --build build \
  --sdk /absolute/path/to/pinned/chibi-sdk --output /absolute/path/to/studio-package
```

On Windows, install the SDK with `cmake --install` from a Windows engine build
(see the engine README) and configure with the same clang and
`-DCMAKE_PREFIX_PATH`; the build writes `build/bin/chibi-dev.exe` with the
SDK's UTF-8 manifest.

The native build writes `build/bin/chibi-dev`. `chibi-dev PROJECT` opens a
project directory or its `project.kdl`; without one it opens the project that
contains the working directory. The SDK's `chibi PROJECT` plays the same project.

Distribution configuration records the SDK package ID and manifest checksum.
Packaging verifies the complete SDK inventory, current compiled inputs and link
dependencies before installing Studio and its resources. The package contains
one application executable. Games consume a separately pinned player package.

Configuration lists missing SDK packages before fetching Studio's pinned Zep
dependency. It does not accept an engine source include directory or fall back to
building the engine checkout. `SDK_HEADERS.json` records the exact external
headers required by the copied editor sources.

The build and install copy the SDK resources into their own `share/chibi` tree.
The executable resolves that tree relative to itself; `CHIBI_RESOURCE_DIR` can
explicitly select a different engine resource root. It does not require the SDK
installation used during compilation to remain at its original path.

To use Studio in any project, install the engine SDK first (`make install` in
the engine checkout), then run `make install` here. It builds a Release Studio
against the SDK in `PREFIX` (default `~/.local`) and installs `chibi-dev` into
`PREFIX/bin`, beside the SDK's `chibi`, which Export Game runs. With
`PREFIX/bin` on `PATH`, `chibi-dev` opens the project in the working directory.
On Windows, run the Makefile's three CMake commands directly.

The [acceptance inventory](tests/acceptance/README.md) describes bounded source
edit/history, keyboard, scene hierarchy/moves, dirty prompts, rendered graph/asset
pointer input, paused-source debugger history and visible-panel construction counts.
Some use queued SDL pointer events; others call production handlers and explicitly
omit hit testing. The pointer selectors explicitly exercise rendered hit targets.
Test source is not a current execution result; final results for the combined
engine/Studio candidate belong in its integration PR.

Recorded evidence includes the October 8 four-frame source edit/undo/redo check
and [PR #53](https://github.com/waldnercharles/chibi-studio/pull/53)'s sixteen
selectors at `243fdc6`, followed by a narrower rerun at `64c0b3b`.
[CI run 38088359470](https://github.com/waldnercharles/chibi-studio/actions/runs/38088359470)
at `d10222d`, using engine `fd69f0bf`, passed bounded starter and Studio-project
startup, installed startup and game export. These results do not establish all
interactions on that revision or full packaged-distribution acceptance.

Studio owns its frame timing log and profiling wrappers. Profiling is disabled
by default. `CHIBI_STUDIO_TRACY=ON` requires an installed Tracy package exporting
`Tracy::TracyClient`; `CHIBI_STUDIO_WEB_TRACE=ON` requires Emscripten and records
Studio zones through browser User Timing. These options profile Studio code;
the installed SDK retains the profiling configuration used to build it.

The browser product consumes an installed Emscripten SDK:

```sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/emscripten-sdk
cmake --build build-web --target chibi-studio
cmake --install build-web --prefix /absolute/path/to/studio-web
python3 tools/serve_web.py --directory /absolute/path/to/studio-web
```

The local server supplies COOP/COEP isolation headers and opens `chibi-studio.html`
at its root URL. The browser bundle preloads SDK shaders,
Studio's UI/project and an empty starter project. Browser projects and thumbnail
caches persist through IndexedDB; import/export and collaboration bootstrap are
owned here. `CHIBI_STUDIO_WEB_PROJECTS` accepts explicit extra sample directories,
each containing `project.kdl` and `content`. Games remain separate repositories.

The SDK exports its worker ABI; Studio selects the matching bootstrap. A serial
site can include a separately built worker site in `threads/`. The serial entry
passes its own same-origin fallback URL. A standalone worker site reports missing
worker/isolation support and can accept an explicit `serial-url` query parameter.
The serial and worker archives must each come from their matching SDK build.
The serial product is built and published by the workflow below.
[Publish run 38088359471](https://github.com/waldnercharles/chibi-studio/actions/runs/38088359471)
built and deployed `d10222d` using engine `fd69f0bf`. Publication is separate
from edit/save/reload, input, export and revision-isolation acceptance in
a browser. The [serial browser report](tests/acceptance/BROWSER.md) records those
interactions on named published/rebuilt revisions. Browser persistence now keeps
unload protection and a reachable Retry until the latest saved bytes are durable,
including concurrent saves and failed creation/import flushes. These records do
not validate the combined candidate or establish threaded/cross-browser acceptance.

## Automatic browser distribution

`.github/workflows/publish.yml` builds the serial editor against the newest
complete engine release, using its installed web SDK and Emscripten 6.0.10.
Changes to browser build inputs on `main` publish automatically. A daily check
also picks up new engine packages and skips compilation when the published
Studio/engine pair is unchanged. Pull requests build without deploying.

The public distribution is at
<https://waldnercharles.github.io/chibi-studio/>. Its `studio.json` identifies
both repository commits and the SHA-256 of an editor archive named for those
commits. Game repositories download that archive and the matching engine export
package during deployment; they do not compile the editor or engine. The archive
contains `chibi-studio.html`, its JavaScript, WASM and data files, and the serial
bootstrap, all at its root. Games can host those files under their own `/dev/`
route with their published project sources.
