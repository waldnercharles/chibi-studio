For recorded serial browser interactions and persistence checks, see
[browser acceptance](BROWSER.md).

The optional native `studio-acceptance` target compiles the actual application and
host in test wrappers. The production binary has no fixture CLI or test hooks.
Build with `-DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON` against an installed SDK.

`studio-acceptance --source-edit --project-root PATH` accepts a disposable copy of
this directory's `project`. Preferences and thumbnail cache are pinned inside that
disposable project; initial Counter bytes are checked before and after. It checks that the project remembered last leads the recent projects, once, and that
Open Project refuses a folder without a manifest. It checks that a failure reports as a warning until the next report. It opens Counter through the normal document loader,
then runs four hidden fixed-dt frames. It calls the production graph expression
commit handler once, applies real undo and redo, and checks current source,
resolved state initializer, owned projection and dirty/history state. With
nothing unsaved, quitting needs no confirmation; with Counter unsaved, the window
title marks it and a quit request asks once, in a toast a repeated request updates.
The final frame renders both graph and code views before normal host shutdown. This proves
consumer edit-handler integration and UI rendering; it does not hit-test pointer
input. Counter is not saved; no scenario, playback or screenshot occurs.

At the end of the fourth frame, a retained-editor check opens one tiny prototype
through Studio's normal history-query dispatch before its relationship exists.
Adding one relationship and refreshing content must clear the same editor's
diagnostic without editing or reopening the source. Text, clean/history state
and the tab checkpoint stay unchanged; another query preserves its syntax cache.
An unchanged executable-source generation ensures source-world rebinding cannot
mask the registry refresh. This adds two files (at most 128 bytes total), one
fixture with two worlds, four history queries, one content refresh and one
validation. It adds no host frames, sleeps or child processes. Files and models
are freed before the final regression assertion.

The first frame also checks prototype and relationship source tabs through
Studio's focused Undo/Redo dispatch, including dirty state and typed values
reopened from the tab's checkpoint. The same two fixtures now use the public
Save dispatch on clean, formatter-canonical prototype and relationship sources:
their exact bytes, history and cleanliness must survive, and a tiny unrelated
scene sentinel must stay unchanged. Dirty Save retains undo and reports an
incompatible relationship as saved but not applied. Undo followed by Save All
saves the restored source, keeps redo and the active tab, and a clean Save All
reports nothing. Invalid drafts reject Save without changing disk and can be
discarded; invalid clean files reject Save with diagnostics and can be closed.
This reuses both fixtures and adds one 36-byte sentinel file
(the two source files are each bounded by 128 bytes), four fixture formatting calls,
eight Save calls and four Save All calls, with six save-triggered content refreshes.
Before reopening the malformed prototype, one explicit Reload adopts its disk
text into the retained workspace source, adding one content refresh and one
CPU-only prerequisite assertion without another file, fixture or rendered frame.
All files and models are freed before the aggregate regression assertion,
including on the old-product control's expected clean-save failures.
One temporary file just above the 64 MiB
prototype collection budget (bounded by 65 MiB) checks that the common asset
reader still reads it while prototype collection rejects it before parsing.
These model checks add no rendered frames; temporary files and models are
released before their final assertions.

The existing first-frame prototype-refresh fixture also opens its registered
sources through the real Assets route and checks the Properties subject with no
previous selection, a different prototype selected, tab reactivation and repeated
opening. One additional malformed source (23 bytes, bounded by 64 bytes) remains
openable with no prototype inspector; opening and reactivating it preserve both
diagnostics. Tab identity/count, source cleanliness, existing prototype history
and the relationship schema in both worlds are retained. This adds two asset-index
rebuilds, one content refresh, four asset opens, two ordinary prototype opens and
four tab activations (one a no-op), with no extra fixtures, worlds or host frames.
Three CPU-only assertions check that passive reads/reloads preserve tab count and
focus, while the deliberate middle-prototype edit adds one background document.
One normal close retains that dirty source before the unchanged exact Assets
reopen-count checks.
All files and models are freed before the separate selection assertion.

After the fourth frame's existing interactions, a relationship source check
saves one tiny temporary declaration through the normal editor path. An
incompatible edit must leave its saved buffer and tab clean while reporting
"Saved, but not applied" and retaining the old schema in both authoring worlds.
An invalid external edit shares a watcher batch with a successful prototype
reload: the owned error record must survive that reload's status message.
Deletion reports the reopen diagnostic, direct reload returns failure, and an
unchanged poll adds no duplicate notice. This uses two files (at most 256 bytes),
one fixture with two worlds, four synthetic watcher times and the same four host
frames. Files and models are freed before the final assertion; copied notices
remain owned by the host kit until ordinary shutdown. This checks notice dispatch
and Output records, not toast pixels or pointer hit testing.

The existing prototype dependency fixture also batches an invalid relationship
edit with a saved Base change. Base, Middle, Leaf and a nested child must all
observe the new value while the live relationship schema stays unchanged and
the owned watcher error survives subsequent refreshes. Duplicate prototype
names cover both-failed and prototype-only failure reporting before restoring
the valid sources. This reuses the four prototype files and adds one relationship
file under 64 bytes, two synthetic watcher times and no host frames. Existing
direct-reload and unsaved-dependent checks remain unchanged; expected negative
results are asserted only after the fixture is cleaned up.

Within the first frame it also opens a tiny temporary draft with two calls and a
branch. It checks that named calls have no duplicate code body and that a small
value call folds into the setter's editable Value row without a Target input.
It connects the calls in a different order and moves one into the branch body
through normal graph gestures. The folded Value pin's context menu opens the
function and returns to its caller, then inlines its body through the source edit
transaction. Palette Get/Set entries create source reads and writes. Members
selects the source declaration, renders its typed Initial property, renames it,
and mounts the actual Members template, dispatches the Variables + button and
consumes its queued Rml activation, adding and selecting a bool declaration while
clearing the Members filter and queuing graph framing and Members-list reveal. It renames the declaration Level through Properties
and changes its
type to Entity through the existing type picker callbacks, with no synthetic
Initial input. A second actual Variables + click with Level selected and filtered
creates and selects an Entity declaration, clears the filter, and is undone.
After source synchronization, the test invalidates cached node identities and
checks that Properties retains Member, then follows an explicitly selected
different graph node. Undo-all also checks that the current presentation contains
only the original integer Count declaration. Each edit is checked and undone, then the clean Counter
fixture is restored before the expression edit above. These interactions
use the same draft, four host frames and three fixture files. The Add binding
uses eight finite panel composition passes inside the first frame, and the draft
starts with an authored comment to exercise lexical body lookup; none adds a
GPU render. The Members list is constrained to64dp to force overflow; two
post-adoption composition/layout passes per Add check that the selected row is
fully visible after its content grows. One additional FrameProbe declaration is left visible after the
first two frames in Both, then deselected and undone in the third frame. The
fourth frame checks that its graph card disappeared and the graph texture
revision changed. This uses the
same four host frames and adds one source edit and undo.

The first frame also checks Discard on malformed behavior, library and system source
tabs, both active and in the background, through `studio_discard_tab`. Their
editors bind the host's real source world. Discard restores invalid disk text
and diagnostics to the working catalog, releases the buffer and tab, preserves
disk bytes and the applied generation, and leaves an unrelated dirty Counter
draft and its overlay intact. An unreadable disk file keeps its draft; an
ordinary Counter Discard still reloads the saved model. This uses three temporary
sources (at most 128 bytes total), six source-only opens/edits/closes, one failed
read attempt and one Counter edit/discard/reopen. It adds no fixtures, worlds,
frames, sleeps or watcher times. Files and buffers are removed before the final
regression assertion; the expected read-error notice remains host-owned until
normal shutdown. The four-frame invocation is unchanged. This exercises the
production handlers, not pointer hit testing.

`studio-acceptance --code-keys --project-root PATH` takes another disposable copy.
It shows Counter in the Text Editor, focuses it and places the cursor after the
event's closing brace, then queues two Enter key presses through SDL, as the
platform does, a few frames apart. The source must gain only whitespace: Enter
types newlines and never accepts a completion nobody asked for, and one undo
takes back both, as typing coalesces. Mod+/ (Command on macOS, Control
elsewhere) on the `count` line then comments it and again uncomments it, and
one undo restores the commented line, an undo step of its own. It runs
twenty-one hidden frames; Counter stays unsaved.

`studio-acceptance --kdl-keys --project-root PATH` takes another disposable copy.
It shows the open scene in the Text Editor, types Enter at the end of its KDL
and saves it: KDL that reads settles without waiting on a graph, so Save writes
it into the disposable copy. Another Enter and a paste follow within the typing
pause, and two undos return to the saved text: Save ends the typing burst and a
paste is an undo step of its own. It runs fourteen hidden frames.

`studio-acceptance --code-followups --project-root PATH` takes another
disposable copy and runs thirty-seven hidden frames on Counter. With Toggle
Line Comment rebound to Mod+C, that chord comments the `count` line instead
of copying, and with Vim on, Mod+/ uncomments it. A typed character, Find
Again (F3) and typing over the match are two undo steps, though the typing
arrives in the frame that reports the caret's move: a command that moves the
caret ends the typing burst at once. A right click opens the text's menu, which
takes the keys: a typeahead letter highlights an item and changes neither the
caret nor the text, and Escape closes it and gives the focus back to the text.
With two problems on one byte, F8 visits both and Shift+F8 goes back. With
Toggle Line Comment rebound to Meta+K, the secondary modifier off a Mac, that
chord comments the problem's line. Counter stays unsaved.

`studio-acceptance --code-glyphs --project-root PATH` takes another disposable
copy, adds a prototype named Crêpes and a UI document, Probe, and runs
thirty hidden frames. In the open scene's KDL, Control+Space in the
strings "Crê" and "Cre" opens the completion list at the same left edge: the
list starts under the word a cell a character, not a byte. In Probe's markup,
with Mod (Command on macOS) held and the pointer resting on a class its styles
do not define, the class is no link. Once another stylesheet defines it and the project's scan
goes stale, it is; held still on it, the class is neither scanned nor looked up
again, and once a same-length class takes its place, the new text is looked up
and is no link.

`studio-acceptance --unit-warnings --project-root PATH` takes another disposable
copy. It shows Counter in the Text Editor and replaces its source with one that
assigns a speed (u/s) to a radius (u). A proven unit mismatch is a compile
error, so by the next hidden frame that line must carry an error underline,
"Units differ: u = u/s", starting at the assignment, and the editor must report
that error. It runs four hidden frames and saves nothing.

`studio-acceptance --scene-hierarchy --project-root PATH` takes another
disposable copy. Its first hidden frame loads a scene whose ship holds a turret
beside a root also named Turret. It checks that the turret loads under the ship
and saves back nested, that the ship can neither be deleted nor change its
components before its turret goes, that the turret keeps its ship through a
component change and its undo, that undoing the turret's deletion puts it back
under the ship, and that a duplicate of the turret becomes the ship's next
child. With the ship turned a quarter
turn, it checks that picking finds the turret at its world position and nothing
at its unturned one, and that a duplicate offset in world space keeps the right
local offset. Its project declares a Link component naming an object;
with two objects selected whose Links name different objects, the third
frame checks that Properties shows Link's Target as a read-only dash and
that clicking it selects nothing, and that once both name one object a
click selects it. A Link the scene names by the path Ship.Turret holds the
turret once loaded, and the path saves again after the object's deletion is
undone, after an edit to the Linker prototype, through a checkpoint, while
the scene runs without A's Link and with the turret taken off its ship, and
after the scene stops, which puts the turret back under its ship; a duplicate
names the turret too. Renaming the turret or its ship moves the paths that
name it, and Undo and Redo, also after a checkpoint, move back only those, so
a path that named the new name before the rename keeps it. Deleted, the turret
leaves the Link naming nothing until that is undone, and the fourth frame
checks that Properties shows the path as a problem.
Two objects naming different paths that resolve to nothing read as mixed. It
saves no file.

`studio-acceptance --scene-physics --project-root PATH` takes another
disposable copy. Its first hidden frame loads a scene with a dynamic 3D crate
and steps 3D physics once. It checks that changing the scene's physics
settings, and undoing the change, drop the crate's 3D body until the next step,
as they do for 2D bodies. It saves no file.

`studio-acceptance --scene-viewport --project-root PATH` takes another
disposable copy and runs nineteen hidden fixed-dt frames. It loads a box with a
spot inside it that nothing draws, and checks that the spot is listed for a
marker and picks above the box, that fitting it for a thumbnail finds nothing
to draw while framing it in the Scene centers on its place, that a resize handle and the rotate ring take
a press a few pixels past them, and that a resize labels its size by the
pointer and cancels back. After Frame All the zoom reads 100%. Through queued
SDL events it checks that a whole wheel notch zooms the camera, that a
fraction of one, as a trackpad scrolls, pans it, that Space with a left drag
pans without playing, that Space released while Settings records a chord
neither stays held nor plays, and that Space alone plays once it is released.
With the box's prototype previewed beside this project's source behavior, the
wheel, a trackpad's fraction included, and Space stay the game's. A prototype
that runs Counter then previews with Counter attached from the source the
preview's world published, and its count rises as the preview ticks.
Framing before the Scene's first draw leaves the zoom's measure to that draw.
While the game plays, the wheel, a trackpad's fraction included, reaches its
UI, and the undrawn spot, which shows no marker then, picks nothing. Stopped,
it loads 300 undrawn spots and checks that the Scene shows a marker for each.
It then narrows the Scene to 240 dp beside the Text Editor and checks that the
rotate tool's hint and its Add button stay inside it, and that after the Move
tool takes over the hint fades out with its text. With the box and spot loaded
again, a marquee dragged through queued SDL events from below the box over the
spot marker's edge, short of its center, selects the spot. It saves no file.

`studio-acceptance --scene-camera --project-root PATH` takes another
disposable copy and runs thirty hidden fixed-dt frames on a 3D scene of
one block. Through queued SDL events it checks that Alt with a left drag
orbits the camera about its pivot, by the drag's distance, selecting nothing
and keeping the zoom readout; that a middle drag pans so the pivot follows the
pointer; that a wheel notch dollies toward the pointer, keeping the point
under it; and that holding the right button with W flies forward without
switching to the Move tool, including a right-button press and W in the same
event batch. A diagonal fractional trackpad scroll checks both axes in a
nonsquare viewport. Focus loss cancels a held right button and W without
release events, and restoring focus leaves the camera still. Frame All then
centers the block along the turned
view, a press on the block above a level view's horizon selects it and drags
nothing, and a view's lens survives a checkpoint, while a checkpoint saved
before views had a lens still reads. It saves no file.

`studio-acceptance --scene-gizmo3d --project-root PATH` takes another
disposable copy and runs forty-two hidden fixed-dt frames on a 3D scene of
a cube a Transform3D places, turned a quarter about Y, a block a Transform
raises, turned by an Orientation, and a gem whose parent turns it a quarter
about Y and mirrors its X. Through queued SDL presses and drags on the gizmo's
handles it checks that Move's X arrow moves the cube along the world's X, and
onto the grid with snapping on, and along the cube's own X in Local Space;
that a quarter turn of Rotate's Y ring, seen obliquely, turns the block's
Orientation by a quarter; and that dragging Scale's X handle out as far again
doubles the cube's own X scale. Also from above, dragging along the X ring,
which shows edge-on as a line, turns the cube a quarter about X, and a
quarter turn of the Y ring turns the mirrored gem a quarter about the world's
Y in place. Rotation is disabled under a nonuniform parent, and a singular
parent offers no editable gizmo. Each drag is one Undo/Redo step restoring the
complete typed fields. Escape and focus loss during a move restore the cube;
hiding the Scene cancels the live gesture without changing history or dirty
state. It saves no file.

`studio-acceptance --scene-collider3d --project-root PATH` takes another
disposable copy and runs twenty-eight hidden fixed-dt frames on a 3D scene of a
crate whose Collider3D box its Transform3D stretches twice along X, a sphere
and a capsule. With the Select tool it drags the active object's collider
handles through queued SDL presses: the crate's -X side out by a quarter of
its stretched width widens the box's size by half, the ball's top raises its
radius, and the capsule's top dragged past its middle, with snapping on,
leaves it no shorter than its two caps. Each drag is one typed Undo/Redo step;
Escape restores a live box edit without changing history or dirty state. A
ray parallel to a handle's axis or a nonfinite box size starts no edit. A hidden
crate takes no drag;
pressing Hide Selected during a live edit cancels it without changing history
or dirty state. Show Colliders then turns on the Scene view's Colliders option.
It saves no file.

`studio-acceptance --scene-cursors` needs no project or renderer. It checks the
cursor each Scene gizmo part asks for, a resize handle's as its box turns.

`studio-acceptance --scene-read` needs no project or renderer. Six pure-C cases
exercise the private, unbound owned scene capture through thirteen calls: eight
accepted captures and five rejections. They check raw actors, prototypes,
component-icon enums, actual assignable field classifications, attachments,
transforms, copied filter/path and editor flags; closed/reopened/nested scopes;
wrong-owner and injected scene-association mismatches; and reads after producer
retirement. One fixed empty behavior is prepared with the bytecode backend;
four source registrations and twelve deferred attachments remain pending with
begin=0. It runs no Begin, authored dispatch, engine frame, UI publication or
child process. This leaf check does not establish queued/paused controller or
live-panel behavior.

`studio-acceptance --graph-visibility --project-root PATH` takes a disposable
copy and runs ten hidden fixed-dt frames. A tiny Counter variant puts a helper
before two handlers: the initial graph still opens on Tick, the picker groups
Events before Variables and Functions, and the root is labeled Variables.
Selecting Begin in Both reveals its declaration; revealing a statement selects
its node and corresponding source while a graph field keeps keyboard focus.
Source reveals and keyboard framing interrupt camera motion immediately; framing
also settles immediately with reduced motion enabled.
Moving the focused source caret to a statement selects its graph/node without
moving the caret back to the declaration or taking focus from the text.
A temporary distant statement tests that initial framing keeps the handler's
entry visible. A behavior without state checks that selecting its empty Variables
root returns the source to the document start, then undo restores Counter. The node position and fixture source are restored. It checks that maximizing Code and
replacing the graph area with Scene stop graph context updates and rendering,
and that showing Both resumes them. Selecting Both from a scene source must
activate the graph's Counter document; returning to the scene preserves its
unsaved source draft and undo/redo history. The accepted collaboration action
changes the document and view together. Hidden graph fields relinquish keyboard
and text input; idle frames retain their source/projection generations and
reference index revision. Two temporary entities also check rotated tilemap
framing, foreground layer priority, hidden-state filtering and selecting empty
cells as part of one layer. They are removed before rendering. No source save or
playback occurs.

`studio-acceptance --graph-labels --project-root PATH` takes a disposable copy
and runs five hidden fixed-dt frames. It opens a graph of two constants whose
values are named, as Name Value does. A long name on a node held to its widest
fits the node but not the room beside its number field: it must clip rather than
paint past the socket and the node's edge, while a short name beside its field
still draws in full.
It saves nothing.

`studio-acceptance --graph-keys --project-root PATH` takes a disposable copy,
adds a Late behavior to it, and runs twenty-five hidden fixed-dt frames on
Counter's Tick graph. It checks that
every output label a full node shows lays out wider than nothing. With the graph
area focused it queues keys through SDL, as the platform does: Right moves the
selection along a wire to the node it feeds, Left to the nearest node feeding
that one, Escape at rest clears the selection, and M asks the source to disable
the selection, which Counter's statement refuses with a reason while the node
stays enabled. With two nodes selected, M refuses before editing either. Escape
that closes the node menu leaves the selection alone. Tab opens node search,
whose entry chosen leads the next search under Recent, also once preferences
are saved and read back, without changing the command palette's recent items.
The last frame opens Late, whose first handler graph is an unnamed completion
handler (its entry an Event node without a name), and checks that it starts on
its Tick graph. Pointer hit-testing and framing are not exercised, and nothing
is saved.

`studio-acceptance --asset-selection --project-root PATH` takes another
disposable copy and adds three unused prototypes to it before Studio opens. Its
hidden frames show the Assets list's prototypes, then click, Shift-click and
Ctrl-click (Cmd on a Mac) rows and extend the selection with Shift and the
arrows, checking the selection after each. Delete then removes the two selected
prototypes' files as one change, and that change's undo puts both back. Counter
stays untouched. A filter the list takes then leaves the grid's arrows on the
tiles it shows. A field of one constant value, added with the spares, then
shows its flat picture in the grid rather than the kind's icon within twelve
frames. A shape whose outline lies outside its frame and a UI document whose
one box paints nothing, added with it, render flat in the grid and keep their
kinds' icons by the twentieth frame. Pointer hit-testing and the Undo toast's
button are not exercised.

`studio-acceptance --export-game --project-root PATH` accepts another disposable
copy. Its first hidden frame runs the Export Game command, which needs the SDK's
`chibi`, `chibi-compile` and `chibi-player` that the build copies beside it. The
second frame waits for the exporter to exit, and the third frame's own poll
reports it. It checks that the command is disabled while the export runs, that
Studio reports the export, and that the reported folder holds the compiled game.
Showing the folder is not exercised.

`studio-acceptance --play-keys --project-root PATH` takes another disposable
copy and runs six hidden frames, each queuing one key through SDL: F5 plays, Space
pauses, F10 steps while paused, F5 resumes and Shift+F5 stops. It also checks the
File menu's Revert File label. It saves nothing.

`studio-acceptance --unsaved-prompts --project-root PATH` takes another disposable
copy and runs at most nineteen hidden frames with reduced motion on, so toasts sit
where layout puts them. Counter's clean tab closes without asking. With Counter
edited, closing its tab asks "Save Counter before closing?" with Save and
Discard, and a click on Discard closes the tab without writing. Edited and asked
again, a Save by hand writes Counter, dismisses the prompt and keeps the tab
open. Then it adds an object to the start scene from a prototype only that run
registers and plays the scene. With Counter edited while it runs, closing its
tab warns only that the edit would be lost, since Counter's text applies live
and still saves. Quit then asks to save both and says to stop the scene to save
them; a click on Save writes Counter and asks
again about the scene the running scene keeps from saving, naming how to save
it. A click on Discard then quits without writing the scene. The clicks are
pointer motion, press and release queued through SDL over the toast's buttons. The initial Counter bytes are
restored afterwards.

`studio-acceptance --panel-idle --project-root PATH` takes a disposable copy
and runs ten hidden fixed-dt frames with two actors. Properties, Assets,
Inspector and BehaviorHeader each synchronize eight times at five points in
editing, playback and stop. The test compares their JSON before and after those
unchanged inputs and reports construction counts and diagnostic timings. It
requires zero closed-menu target/override construction and zero temporary
multiselection lists. Open target choices still refresh every time.

The same run changes selection, edits a numeric property through its production
change action, checks mixed axes for single and multiple selections, opens and
reopens the override menu, filters Assets, and reads
changed Inspector attributes/styles. It clicks the debug-target trigger through
SDL, closes with Escape, opens with Return, and checks rendered entity options
and search focus. Runtime detach/attach while closed must appear immediately on
reopen. The small Assets fixture exercises scroll handling but does not establish
movement through a large virtualized list. These checks establish panel behavior
and work counts, not whole-application or CUT-50 aggregate speedup.

From the engine checkout, after copying `tests/acceptance/project`
as a copy-on-write clone to a disposable directory:

```sh
CHIBI_STUDIO_BUILD=/absolute/studio-build \
CHIBI_STUDIO_IDLE_PROJECT=/absolute/disposable-project \
python3 tools/test_policy.py tool studio-panel-idle
```

The binary comes from this Studio checkout against the installed SDK. The
external project copy and build happen beforehand. This command runs only the
panel-idle selector.

The initial macOS comparison used the same two-actor workload against Studio
`d10222d` and the candidate, with an installed engine SDK from `f3ff608`.
The baseline retained every behavioral assertion and omitted only the new
work-count and initially-empty-menu expectations. Per eight running syncs:

| Construction | Baseline | Candidate |
| --- | ---: | ---: |
| Temporary multi-selection lists | 24 | 0 |
| Closed field-menu override lists | 8 | 0 |
| Closed debug-target choice lists | 8 | 0 |
| Open debug-target choice lists | 8 | 8 |
| Property fields / cached asset rows / Inspector reads | 16 / 0 / 8 | 16 / 0 / 8 |

Assets already reuse their indexed rows. Inspector still reads live DOM and
styles because there is no complete input revision for those values. Timing
output is diagnostic; these small runs support the construction counts, not an
elapsed-time performance claim.

Merely building this target does not run it. A whole UI suite is not implied.
The vendored pico_unit header preserves its upstream license
and original engine vendoring record; it is not installed with the Studio product.

`studio-acceptance --outliner-move --project-root PATH` takes a disposable copy
and runs exactly twenty-one hidden fixed-dt frames with the Outliner maximized. A
three-object 2D scene exercises the real authored rows and SDL pointer events:
dragging a selected group inside a parent, rejecting an ancestor cycle with a
visible rejection mark, and moving before, after and to the scene root. Queued
Alt+Left, Alt+Up and Alt+Right test outdenting, sibling order and reparenting. It
checks stable actor identities, group selection and world-preserving local
coordinates. In its final frame, native context pointer events check that a
behavior drag at the same row edge remains a whole-row assignment target, then
cancel it without assigning an asset. It saves nothing. The separate scene-move case covers persistence,
history and collaboration.

`studio-acceptance --outliner-rows --project-root PATH` takes a disposable copy
and runs exactly two hidden fixed-dt frames. It freezes the existing C
`scene_objects` result before migration: all twelve row fields, native scene
order, filtered windows, clamping, empty results, full-width command-key
resolution, selection, view flags, editability and rename guards. Four actors
form a root/child/grandchild chain and a second root. Real prototype components
supply camera/light icon precedence; those components and Bolt's unassigned
instance `MaterialTerrain` supply all five assignable asset kinds. One Counter
attachment supplies the behavior count. Sixteen direct row queries
materialize twenty-four rows in total. It retains the producer's original
result arenas through a real rename, empty-scene replacement and complete host
shutdown, checking that all row strings and array/type-name headers are owned
before retention. Counter remains unchanged and no scene is saved.

This selector is a native baseline fixture, not a live authored cutover or a
current-generation authority check. It does not exercise pointer input, root
collapse/reveal, an authored policy library or browser parity. The precise
workload, measured native result and migration obligations are in
[Outliner migration parity](../../docs/outliner-migration-parity.md).

`studio-acceptance --outliner-row-policies --source PATH` reads the exact
`projects/studio/content/libraries/OutlinerRowPolicies.chibi` source without
opening a project or starting the host/renderer. One case prepares the library
once for bytecode and once for actual SLJIT, with 41 direct calls per backend:
four depths, nineteen icons, ten drop strings and eight flag records. The 82
calls check ancestor depth, all icon categories and priority/fallback, ordered
unique assignment kinds, selection/view/rename flags and full-width entity
identity. Each call has a 2,000-step budget, at most seven arguments and arrays
of at most six integers. Each backend constructs 33 input integer arrays
containing 68 integers. Backend-entry counters reject compiled-backend fallback.
Each module preparation also evaluates the named glyph-table initializer once
through the existing 100,000-step bytecode preparation budget: two evaluations
total, separate from the 82 direct calls.

The case also checks formatter canonicality/idempotence, unchanged inputs and
owned result headers. It retains the final dynamic drop string and flag record
through source/input retirement without a test-side snapshot; literal string
results are checked while their source module remains alive. It writes no
files, launches no child processes and renders no frames. This checks the
authored helper policies, not live scene capture/currentness, row filtering,
full-width text conversion, production panel cutover or browser parity.

Build with the existing optional acceptance target; its explicit public SDK
dependencies include `Chibi::array_capabilities` and `Chibi::text_capabilities`.
Compilation and execution of this cumulative source are separate checks. Use a
matching installed SDK and binary. The native row baseline and successful
project migration/formatting do not establish this helper route's runtime result.

`studio-acceptance --scene-move --project-root PATH` runs two hidden frames on
an isolated project. It checks moving multiple objects under a rotated parent,
world placement and 2D picking, entity identity, authored and ECS sibling order, one-step undo
and redo with the original selection, and root reordering with a selected parent
and child. Separate disposable worlds check collaboration replay and full
workspace checkpoints, including undo and redo after remapping entity handles
and restoring a deleted moved child. A two-object 3D scene checks all local
transform fields, world placement, undo, redo and checkpoint restoration without
rendering a mesh. Cycles, sibling name conflicts, stale
selection and active runtime sessions preserve the scene and history. It checks
the session guard directly; Play/Stop hierarchy restoration depends on Studio
PR #54 and is not covered by this selector.

`studio-acceptance --graph-pointer --project-root PATH` runs nineteen hidden
frames on Counter. It clicks a rendered node title through SDL, follows its
wire with Right, opens Find with Mod+F, types `count`, and checks Down/Up result
selection and wraparound across graphs. Clicking a result selects its
node; Escape closes Find and then clears the graph selection. Home must bring
every visible node into the viewport from an offscreen camera. Source bytes and
dirty state stay unchanged.

`studio-acceptance --asset-pointer --project-root PATH` runs twenty hidden
frames with four disposable prototypes. SDL input types the Assets filter,
clicks list rows with Shift and Mod, switches to Grid using its button, and
clicks tiles with Mod. It checks exact visible identities and primary/marked
selection for `Spare` and `SpareB`, including switching back to List with the
filter retained. It neither opens nor deletes the assets.

`studio-acceptance --debugger-source-history --project-root PATH` runs
forty-seven hidden frames with one in-memory Counter actor. SDL F9 sets a source
breakpoint and F5 runs to it. Typing invalid source hides the execution highlight
while the applied runtime keeps its paused generation, run, cursor and state.
Mod+Z, Mod+Shift+Z and Mod+Z restore, invalidate and restore the exact text and
highlight. At most ten F10 presses must advance that same runtime through its
source points from count 1 to 2, then Shift+F5
stops it. No source is saved. Valid live edits that replace the runtime generation
are outside this selector.

The engine's `--studio-graph-pointer`, `--studio-asset-pointer` and
`--studio-debugger-source-history` agent GPU routes run these selectors
individually with provenance checks. Each uses a fresh copy of this directory's
fixture. These checks do not establish full acceptance-suite or web input
coverage.

Before each independent engine GPU invocation, restore the bundled manifest:

```sh
cp "$CHIBI_STUDIO_SOURCE/projects/studio/project.kdl" \
  "$CHIBI_STUDIO_BUILD/share/chibi/studio/project.kdl"
```

Studio migrates that build copy when it starts. Restoring it keeps each run on
the same pinned resource bytes; the source project remains unchanged.

`studio-acceptance --project-open` checks project-open failures before UI backend
access. Run it when changing `studio_ui_new` project loading or migration-error
propagation. It uses one pico case and two constructor calls with a zeroed Studio:
an invalid manifest and an invalid version-23 source. Both must return NULL with
the original exact error, including the canonical source path for migration
failure. All three temporary input files (122 bytes total) must remain unchanged,
with no migration staging files. The fixture removes its temporary directory on
normal completion.

This selector returns before host, SDL or renderer initialization. It creates no
window, world or authored execution and runs no host frames. It requires a current
installed SDK with path-qualified migration diagnostics. From the matching engine
checkout, after building this Studio acceptance target against that SDK, run the
single command through the engine runner:

```sh
python3 - /absolute/studio-build/bin/studio-acceptance <<'PY'
import sys
from tools.test_policy import run_commands
raise SystemExit(run_commands([[sys.argv[1], '--project-open']]))
PY
```

Configuration and compilation happen before this invocation. The selector
includes fixture setup, both constructor calls and normal teardown. It does not
run the rendering or interaction routes.

## Canonical prototype source ownership

`studio-acceptance --prototype-source --project-root PATH` is a separate
workload. Run against an isolated copy of the checked-in acceptance project,
with the matching installed SDK that exports `authoring/source_prototype_edit.h`.
It runs two hidden frames. Its first frame uses four tiny prototype
files, three scene actors, and one additional workspace fixture/checkpoint
restore. That fixture prepares its source catalog once from the same fixed
acceptance-project root before restoring the host checkpoint. It creates the
fixture root's `prototypes` directory if absent and
removes only that newly created directory after its four files, including before
the intended negative-control assertion. An existing directory is preserved. No large files, sleeps, new linked probes, or extra rendering passes
are included. It does not expand the existing four-frame `--source-edit` case.

The C/pico case `prototype_source_ownership` exercises ordinary Properties then
Assets open, Assets-first dirty-model conversion, scalar gesture grouping,
omitted/inherited fields and saved-base cache refresh, native component defaults
and a vector axis, structural removal and exact-source Undo, public behavior
attach/detach, raw invalid and identity-changing drafts, a held draft and Save,
source history/checkpoint restoration, Save/Undo/Discard, and background Apply
with inheriting and overridden peers plus source-backed scene Undo/Redo. The one
checkpoint includes populated scene source payloads and a rejected identity
draft. Scene Undo followed by source Undo must recover a later invalid draft.
A missing behavior exercises accepted-but-not-applied status, no-op Apply status,
and reversal of touched behavior attachments. Stale and invalid acceptance
proposals must retain source bytes, typed model, and history.

The same first frame also exercises one notified delete/return of the existing
Apply file. A retained source stays dirty through Close, failed Save All and
Discard, the same checkpoint (now including an invalid missing-file draft),
source Undo, and file return. Only an explicit successful Save resolves the
missing state; no automatic file recreation is tested or allowed. The fixture
then feeds five fixed snapshot/proposal pairs through the existing in-process
collaboration protocol with a counting no-op transport, covering queued palette
add, queued panel removal, and field/behavior command results. One direct and
one replayed no-op field edit check the distinct accepted-but-not-applied result.
There are no sockets, new production hooks, additional frames, worlds, files,
or checkpoint restores. Ordinary missing-path polling and clean unused-asset
deletion are outside this notified-deletion regression.

A failure is not UI interaction proof beyond the explicit command/control paths
listed here. Existing partial runtime refresh and independent scene/prototype
history ordering are not claimed repaired.

The focused negative control keeps the exact engine API/SDK and candidate tests,
but replaces only `edit_prototype`'s canonical field bridge call with the prior
model-only `prototype_write_field` plus `asset->dirty = 1`. The case must fail at
`REQUIRE(source_owned_gesture)` after unlinking its four fixture source files.
This isolates the demonstrated hidden-model Properties regression, rather than
failing because an SDK symbol is absent. Do not run the historical Save/Discard
negative controls again for this bridge. Retain their green four-frame case and
the existing three CPU selectors as adjacent coverage.

One additional edge-only counterfactual restores the prior missing-file dirty
predicate and the two Properties stale-status result branches. It keeps the
retained owner/path and final API declarations, allowing safe continuation.
After normal cleanup the fixture prints both `deleted_source_retained` and
`queued_commands_quiet`, then requires both. The control must report both as 0
and fail that exact final assertion; the clean case reports both as 1. This is
one additional fixed selector run/build, not a replay of historical controls.

## Prototype source host authority

`studio-acceptance --prototype-source-authority --project-root PATH` is a
separate fixed two-frame selector, using an isolated copy of the checked-in
acceptance project. It does not add work to `--source-edit` or `--prototype-source`.
The source fixture uses one temporary
prototype file, one workspace fixture, zero actors, five nonrendering Code
compositions and one real same-workspace checkpoint restoration. Its in-process
transport has no sockets or production test hooks. Including the focused
transport-outcome helper, the fixed workload is 68 incoming envelopes, 96 sends,
30 apply callbacks, 16 restore callbacks (one real workspace restore), two close
callbacks, four direct decoder rejection checks, two offline probe Reloads and
33 tiny retained inputs for the bounded-queue limit.

It checks Code and Properties canonical text/model/history remain unchanged
before guest acceptance; host-own echo applies only once; rapid typing and
successive scalar/vector gestures retain their local values and grouping; exact
accepted outcomes release the tail; stale rejection, disjoint merge, overlap,
explicit Retry Draft and Use Shared preserve recoverable input and the latest
Apply Live intent, including unreported widget input; raw invalid and
identity-changing drafts retain the last-good model; own Undo/Redo validates its
local destination and current Code Apply Live preference while peer history
changes append ordinary local steps; and
pending Save/Undo/Reload, canonical-only snapshots, rebind, interruption and role
loss respect the existing authority boundary. Retry Draft is an explicit primary
control and does not take editor focus or consume a newline.

After the original message-count guards, the same fixture accepts two peer
actions with opposite local Apply Live preferences and calls the production
`studio_code_frame` update after each. Accepted `live=false` remains held with
the local preference enabled; accepted `live=true` remains live with it disabled.
Two further nonrendering Code updates poll one overlapping external edit of the
same file. Its diagnostic remains visible without changing canonical text,
model or history; one explicitly accepted Reload then clears the conflict and
adopts the disk bytes. These four Code updates add no rendered frames, actors,
files or checkpoint restores. The final diagnostics report `held=1, live=1`
and `conflict=1` only after cleanup.

Two separate counterfactuals use this unchanged authority selector. The frame
control restores only the prior per-frame local-preference interpretation and
must report `held=0, live=0` before failing the final frame-intent assertion.
The disk control drops only the managed refresh failure diagnostic and must
report `held=1, live=1` and `conflict=0` before failing the final disk assertion.
Each preserves every earlier authority, held-retry and source/model/history
check; unrelated early failures do not count as either negative proof.

When `CHIBI_STUDIO_CONTROLS_CAPTURE` names a fresh PNG, the same authority
invocation also presents the real retained-draft notice in the host workspace.
After frame one's original fixture has cleaned up, it creates one 53-byte public
prototype source in the disposable project, submits one local proposal as an
editor guest and rejects it through the in-process transport. Two finite
nonrendering compositions call the actual `studio_emit` under its real workspace,
area and document keys. Each existing composition resolves styles/layout once
without adding a host/GPU frame or sleep, so the next emission reads styled Code
swatches. The fixture applies its reduced-motion choice through the normal kit
theme API before composition; the host normally applies preferences afterward.
A fixed public status label replaces the temporary fixture path in the image.
Code takes focus before rejection; adding the notice
must leave that exact element focused. One queued Enter is consumed by Code in
the existing second frame, adding only a local newline while canonical bytes,
history and disk stay unchanged. Neither Retry Draft nor Use Shared is activated.
The same notice/conflict identity remains after the real host's final framebuffer
readback; its file and transport allocations are removed after host shutdown.
This adds one prototype, two incoming messages and two outgoing messages, zero
actors, zero checkpoints, zero host/GPU frames, sleeps or child processes. The
whole selector with capture is 70 incoming messages and 98 sends; without capture
it remains 68 and 96. The screenshot uses the existing `--screenshot` path and
its existing readback limit.
The screenshot contains only the disposable Studio fixture. Independently inspect
its actual Retry Draft primary/Use Shared secondary layout, text baselines,
spacing and readable surrounding kit controls; capture success alone is not
visual approval or browser verification.

The one authority counterfactual optimistically commits a guest's prepared source
inside the common pending boundary while retaining normal transport sequencing.
The dedicated case must print `Code unchanged=0, Properties unchanged=0` and fail
at `REQUIRE(test.code_authority && test.property_authority)` after complete
fixture and collaboration cleanup. This independently demonstrates both reported
entry gaps without bypassing the SDK, dropping later positive assertions or
changing the frame/message limits. Candidate success requires both booleans 1
and all remaining source/model/history assertions. Source review does not
establish runtime success; native and web builds and the hosted selector results
are reported separately.

The separate held-intent counterfactual reverses only Retry's apply-policy lookup
from the retained draft to the accepted source's runtime hold. It keeps the
compiler helper rename, managed-widget capture, Code history fix and all final
fixtures. The same authority selector must reach its held rejection/Retry phase,
then print `Code unchanged=1, Properties unchanged=1` and `retry_held=0` after
cleanup before failing at `REQUIRE(test.retry_held == 1)`. This is separate from the
original authority control, whose early exit prints `retry_held=-1` (not run);
candidate requires `retry_held=1`. It adds no frames,
transport envelopes or actors to either invocation.
