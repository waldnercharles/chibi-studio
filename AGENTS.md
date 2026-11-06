# Chibi Studio

Studio is an authored editor. RML defines structure, RCSS defines appearance,
and `.chibi` owns commands, selection, panel state and workflows. Reusable
compiler, runtime, document, rendering, platform and input capabilities belong
in the engine repository and are consumed through a packaged, installed SDK.
Read the engine's `AGENTS.md` before changing shared capabilities.

## Ownership

- Keep Studio free of C/C++ implementation and native build targets. Author
  missing editor behavior in `projects/studio/content/behaviors` and shared UI
  compositions in `projects/studio/content/ui`. Add reusable SDK capabilities
  to the engine when the authored language needs them.
- Build against an installed SDK, never the engine source tree or its private
  headers. Package the engine first after an engine change. The compiler,
  generic author host and runtime metadata must come from the same package.
- Follow the engine's test budget, quarantine, review and autonomy rules.
  A source compilation check, CPU service check, startup check, rendered
  interaction, package check and browser acceptance establish different things.
  Report their scope separately.
- `EXTRACTION.json` records copied source origins. Update `consumer_sha256` when
  an active extracted file changes. Retired native entries retain their origin
  and Git baseline; they are historical records, not files to restore.
- Studio's bundled projects are Chibi projects. Migrate their format with the
  installed `chibi_ctl --migrate` when needed for the actual change, then format
  with `chibi_ctl --format`. Avoid version-only changes.

## Build and validate

Package the SDK from a sibling engine checkout, then build Studio:

```sh
cd ../chibi
make package-sdk SDK_PREFIX=dist/sdk
make studio-build STUDIO_SOURCE="$PWD/../chibi-studio" STUDIO_SDK_PREFIX=dist/sdk
```

The launcher is `build-studio/bin/chibi-dev` in that engine checkout. It accepts
an explicit project directory, `project.kdl`, or `--workspace-root`; without a
project it searches working-directory ancestors and otherwise opens Home.
See `README.md` for standalone, installed and browser builds.

Use bounded startup and interaction fixtures through the SDK's generic host.
The engine's `docs/testing/ui-replay.md` describes replay, assertions, capture
and evidence identity. Studio fixtures live in `tests/acceptance`. Historical
native test results and held oracles do not establish current acceptance; keep
those claims labeled and preserve their Git provenance when retiring them.

## Editor design

For UI and interaction changes, read the
[UX laws skill](.claude/skills/ux-laws/SKILL.md), the engine's
`docs/prototypes/studio/DESIGN.md` and `docs/editor-ui-references.md`.
The graph editor also follows `docs/editor-graph-references.md`.

Inspect comparable authored controls before adding or changing a component.
Reuse shared field, picker, popup, menu, dialog, notice and layout components.
State belongs in authored behavior; represent interaction states with `data-*`
attributes and style them through shared RCSS tokens. Native engine controls
provide reusable input mechanics, while Studio owns their composition and
workflow.

Use controls that match reflected types: checkboxes for booleans, numeric
editors for quantities, color pickers for colors, and asset pickers for
references. Represent choices as named enums with searchable dropdowns that
include every value and show at most seven options at once. Keep field labels,
values and icons aligned. Graph constants and bindings remain editable both
in the node and in Properties. A type-aware source field handles literals and
compatible property paths without a separate source-kind selector.

Before calling an interface finished, inspect rendered text baselines, icon
alignment, spacing, grouping, focus, disabled and error states. Exercise the
actual source edit, undo/redo and panel workflow. Validate browser behavior on
the combined browser package; native startup alone cannot establish it.
