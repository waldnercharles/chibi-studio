Chibi Studio is the editor for the Chibi engine. It builds against an installed
Chibi SDK and never against the engine's source tree. Engine, compiler, runtime
and shared UI backend code belong in the engine repository
(`waldnercharles/chibi`); fix them there and consume the new SDK here.

## Rules

- Code style, testing, review-before-commit and autonomy rules are the engine's:
  follow `AGENTS.md` in the engine repository (`../chibi/AGENTS.md` when the two
  are checked out side by side). Write C, never C++; `src/editor/text/zep_bridge.cpp`
  is the one pass-through shim.
- Studio's own projects (`projects/starter`, `projects/studio`,
  `tests/acceptance/project`) are Chibi projects. They may lag the engine's
  project version: Studio upgrades an older project when it opens it, and CI
  opens the first two against engine `main`. Never open a PR that only bumps their
  version. Migrate one with the engine's `chibi_ctl --migrate` in a PR that
  changes it for another reason, and format it with `chibi_ctl --format`.
- `EXTRACTION.json` records each file copied from the engine. After editing one
  of those files, set its `consumer_sha256` to the new file's SHA-256 in the
  same commit. New Studio-only files are not listed there.

## Build and run

From an engine checkout next to this one, package the SDK and build Studio
against it:

```sh
cd ../chibi
make package-sdk SDK_PREFIX=dist/sdk
make studio-build STUDIO_SOURCE="$PWD/../chibi-studio" STUDIO_SDK_PREFIX=dist/sdk
```

That writes `build-studio/bin/chibi-dev` in the engine checkout. To build in
this repository instead, see `README.md`
(`cmake -DCMAKE_PREFIX_PATH=<sdk> ...`, output `build/bin/chibi-dev`).

Smoke-test a build with two hidden frames. On Linux without a display, prefer
SDL's supported `offscreen` video driver when the installed SDL/Vulkan stack
supports it. `--headless` only hides the window; it does not select a video
driver. A working software Vulkan implementation such as Mesa llvmpipe can
render without Xvfb. See [offscreen rendering](docs/offscreen-rendering.md) for
prerequisites, isolated project/resources, the unchanged 60-second supervisor,
and actual screenshot verification. From the engine checkout, the underlying
bounded command is:

```sh
env SDL_VIDEO_DRIVER=offscreen SDL_AUDIODRIVER=dummy build-studio/bin/chibi-dev \
  --project-root /path/to/isolated/project --headless --frames 2 \
  --fixed-dt 0.016666666666666666 --screenshot /path/to/studio.png
```

Keep the entire invocation inside the production supervisor. If the selected
SDL/backend lacks offscreen support, report its error; do not silently retry or
increase frames/deadlines. An explicitly selected, supported Xvfb/X11 setup is a
separate fallback requiring a usable graphics backend. Xvfb itself is not one.

It must exit 0, and its log must not contain `Studio UI:`, `runtime frame failed`
or `Could not initialize`. Two frames are a startup check, not UI coverage; say
which interactions you exercised. The opt-in `studio-acceptance` target
(`-DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON`) adds a source-edit, undo and redo check;
see `tests/acceptance/README.md`. CI (`.github/workflows/ci.yml`) runs this
build and smoke test against the engine's `main`.

## Formatting

Studio uses the engine's clang-format 22 style
(`pip install 'clang-format>=22,<23'`). Much of the existing source predates
it, so format only the lines you change:

```sh
git clang-format --style=file:../chibi/.clang-format
```

## Editor UI

For UI/UX design, interaction reviews, or editor usability work, follow the
[UX laws skill](.claude/skills/ux-laws/SKILL.md).

Studio's UI is RmlUi, driven from C through `src/editor/ui/kit.h`. Its design
is in the engine repository's `docs/prototypes/studio/DESIGN.md`. Before
building any UI mechanism in a panel, read the engine's
`docs/editor-ui-references.md`. It maps each reference library to the kit
primitive that solves the same problem:

- Base UI, shadcn/ui, cmdk and DialKit
- Motion, NumberFlow, Torph and Sonner
- Virtuoso, dnd-kit, Shiki, Liveline and Recharts
- zustand, next-themes, clsx and cva

The behavior graph editor follows the engine's `docs/editor-graph-references.md`
(xyflow, imgui-node-editor, Blender, Unreal, ComfyUI and others).

Use the kit primitive. If it is missing, add it to the kit following that
library's contract instead of building a one-off in the panel. Before changing
a component, inspect and reuse comparable controls already in Studio. Before
calling it finished, compare its text baselines, icon alignment, spacing,
grouping and interaction states with those controls in a rendered view.

Pane and graph fields must use controls that match their reflected type:
checkboxes for booleans, numeric editors for quantities, color pickers for
colors, and asset pickers for references. Integers that represent choices must
be named enums shown as searchable dropdowns, including every enum value.
Anchor dropdowns to their field, and keep labels, values and icons vertically
aligned. Graph inputs, bindings and folded sources must remain editable in their
node and in Properties. Use one editable field with type-aware autocomplete for
graph sources: typing a literal sets a constant, and typing a property path
browses compatible fields. Do not add a separate source-kind picker for actions
the same field can handle. Group input controls by device type and reuse the
Studio icon set.

Common mismatches to catch:

- Toasts built by hand, or a modal used for a notice: use `kit_toast` (Sonner).
- Hand-written open, close, focus or outside-press logic: use kit popups, menus and the command palette (Base UI).
- A number animated by rewriting its text: use `kit_number_flow` (NumberFlow).
- 1,000 or more rows emitted directly: use `kit_list` (Virtuoso).
- Shared state threaded through parameters or copied into statics: authoring state stays in the C model, and view settings go in `studio_prefs` (zustand).
- Class strings built with ternaries: use `KIT_CX` (clsx) and `kit_variant`/`kit_size` (cva); state goes in `data-*` attributes.
- Instant show and hide, or motion interpolated by hand: use `kit_presence` and `kit_layout` (Motion).

Verify UI changes in the web build (`make web STUDIO_SOURCE=...` from the engine
checkout).
