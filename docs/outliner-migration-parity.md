# Outliner migration parity contract

## Status and baseline

The `--outliner-rows` acceptance selector is a **baseline fixture** for the
current C `STUDIO_NODE_SCENE_OBJECTS` service. Adding this fixture does not
replace the production producer, alter `OutlinerRows.chibi`, or admit a test
invocation. Its expectations were derived from the unchanged product sources
at Studio `07dccf7b5fcfcc879b9d24e95730f4ee1d6a281f` (tree
`2f66ad47f0f13437f9c86dda1d7921ccd0b3c369`).

On 2026-10-16 the corrected fixture at Studio code commit
`a8062aff75dae644ad0f0a1f1ec656e4a3bf8dfb` passed against the installed native SDK
from engine `c4a9ed52f89da40c837d03222a682fb946909f62`: **1/1 case, 1,186 assertions,
two frames, 16 queries and 24 rows in 4.231 seconds**, under the unchanged
60-second whole-command supervisor. The acceptance-only Release/distribution
build, copied resources, SDL offscreen/Mesa providers and immutable postflight
were independently checked. This establishes the native current-service
baseline only; no browser, performance or authored live-cutover pass is claimed.

The original fixture at `86e3631d5537087598b916ef95d54129411ebf5e` failed during
prototype registration, after the initial empty-scene query and before the full
row matrix. It incorrectly put an unbound `MaterialTerrain` on a registered
prototype, whose preparation requires an existing procedural field. The
correction moved that real component into Bolt's authored instance components,
where an unassigned field is valid editable state. All expected rows, assertions
and workload bounds were retained. The failed source, binary, raw log and
isolated scratch were preserved alongside the corrected result.

The authoritative implementation references are:

- `src/editor/studio/studio_node_calls.c`: row construction and request clamping.
- `src/editor/studio/studio_outliner.c`: filter order and current entity lookup.
- `src/editor/studio/studio_scene.c`: byte-oriented matching and editability.
- `src/editor/studio/studio_shell.c`: component icon priority and glyph mapping.
- `src/editor/studio/studio_drop.c` and `src/editor/workspace/scene_asset_drop.c`:
  ordered, actually assignable asset kinds.
- `projects/studio/content/behaviors/OutlinerRows.chibi` and the existing
  `Outliner.rml`: root/list controller behavior and consumers of the row keys.

Keep these expected values when replacing the producer. A failed assertion is
a parity regression to explain and fix, not a reason to regenerate expected
values from the new implementation.

## Fixed native row contract

The four live actors are `Alpha`, `Bolt`, `Élan`, `Delta`, in that scene order.
The first three form a root/child/grandchild chain; Delta is a second root.
Expected values are literals in `tests/acceptance/outliner_rows.h`, rather than
another implementation of the C policy.

| Field | Baseline expectation |
| --- | --- |
| key | Exact unsigned decimal ECS entity bits; distinct from persistent actor identity. The real producer's four keys resolve to their current actor indices. |
| label | Alpha, Bolt, Élan, Delta; the renamed result has Bolt-new only. |
| icon | Prototype fallback, camera, light, prototype fallback. CameraRig contains a light before its camera and a system component; Bolt also has an instance terrain component. Enum priority, not source order, chooses the camera glyph. |
| prototype | Plain, CameraRig, Lamp, Plain. |
| drop | `object behavior` for Plain/Lamp; `object behavior material shape ui field system` for Bolt (CameraRig plus instance terrain); empty for every read-only row. |
| depth | 1, 2, 3, 1, even when a filter omits ancestors. |
| behaviors | 0, 1, 0, 0 from the real runtime source registry's Counter attachment. |
| active | Only Élan. |
| marked | Bolt and Delta; the active row is not also marked. |
| hidden | Bolt and Delta. |
| locked | Élan and Delta. |
| renaming | Bolt only while the Outliner rename flag is true and its complete entity matches. |

Bolt's assignable fields use the actual reflected `Shader.material`,
`Shader.shape`, `Ui.document`, `MaterialTerrain.field` and `EngineSystem.asset`
classifications. `MaterialTerrain` is an unassigned authored instance component;
the other components come from CameraRig. No procedural field is compiled or
baked. Merely naming a component or asset is not equivalent. These
components are inspected synchronously and the scene is replaced before the
next host frame; this fixture does not execute their rendering or system work.
The read-only check sets and restores the existing runtime-snapshot flag inside
that same synchronous test region. It is not Play/Stop coverage.

Each result checks the array type/container/count/capacity, owned nominal type
name, typed single-row headers, row storage and all five string addresses
before any retention. There is no test-side result snapshot or string copy.
This native reflected row uses the existing `SceneObjectRow` schema. An
additional authored record descriptor, if later introduced, needs its own
complete owned-header checks; this fixture does not establish those by analogy.

## Frozen workload for independent admission

One case, one disposable copy of the existing three-file acceptance project,
one normal host startup/shutdown, exactly two hidden 1920×1080 frames at fixed
`0.016666666666666666` seconds, no screenshot or queued input. The first callback
performs all scene operations; the second checks the empty replacement scene.
There are three in-memory prototype registrations, one authored instance
component, at most four live actors, one Counter attachment, one rename and one
empty-scene replacement. No scene
or behavior file is saved. Existing host initialization may migrate copied
project/resources; preserve and inspect that separately.

Exactly **16 direct row-service calls materialize 24 rows**:

| Phase/request | Calls | Materialized rows |
| --- | ---: | ---: |
| Initially empty scene | 1 | 0 |
| Full initial result, retained | 1 | 4 |
| First=1, Count=2 | 1 | 2 |
| First=3, Count=9 | 1 | 1 |
| First=-2, Count=2 | 1 | 2 |
| Negative Count; First beyond end; zero Count | 3 | 0 |
| Name-only ALP; prototype-only camera; no match; literal UTF-8 É | 4 | 3 |
| Read-only full result | 1 | 4 |
| Outliner rename flag off | 1 | 4 |
| Renamed full result, retained separately | 1 | 4 |
| Empty replacement scene | 1 | 0 |

Shown is checked independently of the requested window. Empty results are
still typed and owned. Both retained originals are checked after the rename,
scene replacement and complete host/scene/world teardown. Replacing the scene
makes all four old keys fail current lookup while their historical rows remain
readable. Historical row ownership is not permission to use it against a new
scene.

Eight separate resolver calls use four synthetic actor entries with full-width
handles, including `9007199254740993`, two same-slot/different-generation
handles and a high-bit-set value. Their persistent actor identities are 11–14.
They check all four literal decimal keys, persistent-identity mismatch, NULL,
current reordering and removal. No synthetic entity is passed to a live ECS
world. Eight more resolver calls check the four real keys before and after
scene replacement. This distinguishes the command-key resolver from the
engine's existing source-backed `test/language/outliner_projection.c` coverage
of full-width formatting and authored value lifetime.

These counts describe explicit fixture calls, not every call the normal
application makes while starting or composing its panels. They are not host
crossing, allocation, renderer or compiler performance measurements.

Before each execution, independently review the exact fixture, dispatcher,
installed SDK, binary/resource provenance and renderer environment. Build time
is separate. After the current unchanged Studio rendering baseline is verified,
run only the reviewed `studio-acceptance --outliner-rows --project-root PATH`
invocation inside the unchanged production `test_policy.run_commands` supervisor
with a single 60-second deadline including children, initialization, both frames
and teardown. Preserve the exact SDL offscreen/Mesa environment and isolated
project/resources described in [offscreen rendering](offscreen-rendering.md).
This new selector is not an existing admitted engine `agent.py` route; do not
borrow another selector's label or bypass admission.

Require exit 0, the exact success marker
`Studio Outliner rows accepted: frames=2 queries=16 rows=24`, unchanged Counter
bytes, no `Studio UI:`, `runtime frame failed` or `Could not initialize` marker,
and no surviving process. Keep logs and source/SDK/resource identities. Stop on
failure; do not increase frames, retry with a different backend, weaken
assertions or reconstruct a larger suite from chunks.

## Separate obligations for the authored live conversion

The baseline intentionally does not claim:

1. A live raw-fact capture/currentness contract. Validate owned names, complete
   type headers, entity/parent order and auxiliary facts before publication.
   Reject malformed hierarchy and stale capture without changing prior
   published state. Audit the actual callback, reentrancy, pause/suspend and
   property/UI mutation boundaries. A refresh stamp or historical owned result
   is not proof of current scene authority.
2. Authored policy execution. Exercise the actual library's icon categories,
   priority/fallback, each assignment kind, depth and state controls separately,
   with the existing engine projection as complementary evidence. No public
   conversion/Contains API is selected by the baseline.
3. Root/list controller parity. Preserve root depth 0, root-inclusive First/Count
   adjustment, expanded total 1+Shown, collapse total 1, notice/filter text and
   reveal index+1. Check two mounted views with independent window/collapse state
   only when that layer changes. Do not substitute arithmetic in this native
   fixture for executing the actual authored controller.
4. Pointer/keyboard, command/history or browser parity. The existing
   `--outliner-move` and `--scene-move` retain their original assertions and
   finite bounds. Current-SDK execution of those selectors and later browser
   verification are separate gates, not implied by these two frames.
5. Every metadata variant. Missing/unresolved prototypes, every icon category,
   independent optional assignment combinations and multiple attachments need
   complementary targeted coverage. No successful result for one mixed
   prototype proves those all work.

Keep these gates distinct in a migration PR. Baseline coverage can land
independently only after its own required review and validation; it cannot be
reported as completed live panel migration.
