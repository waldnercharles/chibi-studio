# Chibi Studio

Studio is an authored Chibi application. RML defines its structure, RCSS defines
its appearance, and `.chibi` defines editor behavior, commands, selection and
workflow state. It consumes packaged SDK tools through a `LANGUAGES NONE` CMake
project. Reusable document, compiler, renderer, platform and input services live
in the engine. Studio contains no C or C++ sources; retired implementations
remain recoverable through the Git baseline recorded in `NATIVE_RETIREMENT.json`.

## Migration candidate status

The engine's [canonical migration evidence](../chibi/docs/studio-rml-migration.md#current-candidate-evidence-2026-10-14)
records SDK/source identities and scoped validation results.
SDK13 CPU and native/Web package checks passed. Current source v6 canonical
formatting passed; final application and interaction results remain separate.
Native ownership retirement covers 347 files, including 318 C/C++ source/header
files. Source retirement does not certify complete behavior parity.

Build against an installed SDK:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIBI_STUDIO_DISTRIBUTION=ON \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/pinned/chibi-sdk
cmake --build build
build/bin/chibi-dev
build/bin/chibi-dev /absolute/path/to/project
```

The launcher opens a supplied project directory or `project.kdl`. Without a
project argument it searches the working directory's ancestors; if none contains
a project, Studio opens Home. Windows uses `build/bin/chibi-dev.cmd` and the
SDK's native executables. Studio itself needs no C/C++ compiler.

To install beside an installed engine SDK, run `make install PREFIX=/path/to/prefix`.
To make a relocatable distribution:

```sh
python3 /absolute/path/to/pinned/chibi-sdk/share/chibi/tools/package_studio.py \
  --build build --sdk /absolute/path/to/pinned/chibi-sdk \
  --output /absolute/path/to/studio-package
```

Packaging verifies the SDK identity and inventory, the prepared application and
its source resources. The distribution includes the generic author host,
compiler and player tools, authored Studio content, and the exact SDK metadata,
shaders and licenses. The launcher resolves these beside itself.

A bounded startup check uses the generic host's flags:

```sh
build/bin/chibi-dev --hidden --frames 2 --fixed-dt 0.0166667
```

On Linux without a display, use the engine's documented bounded display adapter.
Startup is separate from interaction coverage. The SDK's generic UI replay
protocol dispatches ordinary pointer, key and text input, resolves real hit
geometry, asserts visible UI/data, and records source/SDK/backend/capture identity.
See the engine's `docs/testing/ui-replay.md`; Studio's first replay fixture is
`tests/acceptance/studio_home_replay.json`. Its presence does not establish a
passing execution result. Historical native acceptance remains documented in
[the historical acceptance inventory](tests/acceptance/HISTORICAL_NATIVE.md); broad quarantined suites
must not be reconstructed as separate chunks.

Browser builds consume matching installed Emscripten SDK and generic web-author
products:

```sh
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/emscripten-sdk \
  -DCHIBI_AUTHOR_WEB_DIR=/absolute/path/to/matching/web-author
cmake --build build-web --target chibi-studio
cmake --install build-web --prefix /absolute/path/to/studio-web
python3 tools/serve_web.py --directory /absolute/path/to/studio-web
```

Build those products in the engine with `make package-web-sdk package-web-author`.
The browser app opens Home by default. `CHIBI_STUDIO_WORKSPACE_ROOT` optionally
seeds one project; `CHIBI_STUDIO_COLLAB_URL` selects a trusted HTTP(S) room service.
Project archives and target compilation use generic host adapters and an isolated
compiler worker. Browser persistence, worker execution, room behavior and visual
interaction require separate fresh acceptance on the combined package.

`EXTRACTION.json` records copied source origins and hashes. Contributor rules are
in [AGENTS.md](AGENTS.md). Held test oracles and previous reports are historical
coverage, not passing results for this migration.

## Earlier validation

Recorded evidence includes the October 8 four-frame source edit/undo/redo check
and [PR #53](https://github.com/waldnercharles/chibi-studio/pull/53)'s sixteen
selectors at `243fdc6`, followed by a narrower rerun at `64c0b3b`.
[CI run 38088359470](https://github.com/waldnercharles/chibi-studio/actions/runs/38088359470)
at `d10222d`, using engine `fd69f0bf`, passed bounded starter and Studio-project
startup, installed startup and game export. These results do not establish all
interactions on that revision or full packaged-distribution acceptance.
