# Chibi Studio

This repository owns Studio's editor, document and workspace code, its application
loop, retained UI kit, text editor bridge, desktop host, and authored Studio UI.
It consumes an installed Chibi SDK. Engine, compiler, renderer, shared UI backend
and native runtime implementations remain in their owning SDK packages.

`EXTRACTION.json` records every copied source's original path and SHA-256 hash.
The original files are held in the engine repository through installed-consumer
acceptance. Preserved test oracles live in `tests/held/engine_oracles`. There is no remote.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIBI_STUDIO_DISTRIBUTION=ON \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/pinned/chibi-sdk
cmake --build build
python3 /absolute/path/to/pinned/chibi-sdk/share/chibi/tools/package_studio.py --build build \
  --sdk /absolute/path/to/pinned/chibi-sdk --output /absolute/path/to/studio-package
```

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

The standalone application has compiled and linked against an installed SDK.
Two hidden startup frames passed. A separately compiled acceptance target passed
four hidden frames with one real source edit, undo back to the clean original,
and redo, checking source, semantic facts and the graph projection. This covers
the production edit/history handlers and UI rendering; pointer hit testing, web
execution and final packaged-distribution acceptance remain unverified.

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
The serial browser product has compiled and linked against the pinned Emscripten
SDK. Browser interaction and the threaded product remain unverified.
