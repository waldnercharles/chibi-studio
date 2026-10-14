Chibi Studio is the editor for the Chibi engine. It builds against an installed
Chibi SDK and never against the engine's source tree. Engine, compiler, runtime
and shared UI backend code belong in the engine repository
(`waldnercharles/chibi`); fix them there and consume the new SDK here.

## Rules

- Author the editor in RML, RCSS and `.chibi`. The target is a Studio project
  with no C/C++ editor implementation. Existing native code is migration debt;
  missing reusable capabilities belong in the engine SDK and must be callable
  from `.chibi`. For editor features or migrations, read
  [the authored editor target](../chibi/docs/studio-ui-authoring.md#authored-editor-target).
- Testing, review-before-commit and autonomy rules are the engine's:
  follow `AGENTS.md` in the engine repository (`../chibi/AGENTS.md` when the two
  are checked out side by side). Repairs to existing native code follow its C
  style; `src/editor/text/zep_bridge.cpp` is the existing pass-through shim.
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

Smoke-test a build with two hidden frames. On Linux without a display, wrap it
in `xvfb-run -a` and set `SDL_AUDIODRIVER=dummy`. From the engine checkout:

```sh
xvfb-run -a env SDL_AUDIODRIVER=dummy build-studio/bin/chibi-dev \
  --project-root ../chibi-studio/projects/starter --headless --frames 2 --fixed-dt 0.0166667
```

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

Studio's UI uses RmlUi. Its existing C kit (`src/editor/ui/kit.h`) is a reference
for the behavior of controls during migration. Its design is in the engine
repository's `docs/prototypes/studio/DESIGN.md`. Before
building any UI mechanism in a panel, read the engine's
`docs/editor-ui-references.md`. It maps each reference library to the kit
primitive that solves the same problem:

- Base UI, shadcn/ui, cmdk and DialKit
- Motion, NumberFlow, Torph and Sonner
- Virtuoso, dnd-kit, Shiki, Liveline and Recharts
- zustand, next-themes, clsx and cva

The behavior graph editor follows the engine's `docs/editor-graph-references.md`
(xyflow, imgui-node-editor, Blender, Unreal, ComfyUI and others).

Reuse the authored components in `projects/studio/content/ui` and their `.chibi`
behaviors. Add missing shared component compositions there; add native
capabilities to the engine when authored code needs them. Before changing
a component, inspect comparable controls already in Studio. Before
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

When repairing existing native kit code, also catch:

- Toasts built by hand, or a modal used for a notice: use `kit_toast` (Sonner).
- Hand-written open, close, focus or outside-press logic: use kit popups, menus and the command palette (Base UI).
- A number animated by rewriting its text: use `kit_number_flow` (NumberFlow).
- 1,000 or more rows emitted directly: use `kit_list` (Virtuoso).
- Shared state threaded through parameters or copied into statics: authoring state stays in the C model, and view settings go in `studio_prefs` (zustand).
- Class strings built with ternaries: use `KIT_CX` (clsx) and `kit_variant`/`kit_size` (cva); state goes in `data-*` attributes.
- Instant show and hide, or motion interpolated by hand: use `kit_presence` and `kit_layout` (Motion).

Verify UI changes in the web build (`make web STUDIO_SOURCE=...` from the engine
checkout).
