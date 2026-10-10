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
input. No source save, scenario, playback or screenshot occurs.

`studio-acceptance --code-keys --project-root PATH` takes another disposable copy.
It shows Counter in the Text Editor, focuses it and places the cursor after the
event's closing brace, then queues two Enter key presses through SDL, as the
platform does, a few frames apart. The source must gain only whitespace: Enter
types newlines and never accepts a completion nobody asked for, and one undo
takes back both, as typing coalesces. Control+/ on the state line then
comments it and again uncomments it, and one undo restores the commented line,
an undo step of its own. It runs twenty-one hidden frames; Counter stays
unsaved.

`studio-acceptance --kdl-keys --project-root PATH` takes another disposable copy.
It shows the open scene in the Text Editor, types Enter at the end of its KDL
and saves it: KDL that reads settles without waiting on a graph, so Save writes
it into the disposable copy. Another Enter and a paste follow within the typing
pause, and two undos return to the saved text: Save ends the typing burst and a
paste is an undo step of its own. It runs fourteen hidden frames.

`studio-acceptance --unit-warnings --project-root PATH` takes another disposable
copy. It shows Counter in the Text Editor and replaces its source with one that
assigns a speed (u/s) to a radius (u). By the next hidden frame that line must
carry a warning underline, "Units differ: u = u/s", starting at the assignment,
and the editor must report no error: the source still runs. It runs four hidden
frames and saves nothing.

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
local offset. It saves no file.

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
With a Preview target playing, which it sets directly because this project's
source behavior keeps a prototype preview from launching, the wheel, a
trackpad's fraction included, and Space stay the game's.
Framing before the Scene's first draw leaves the zoom's measure to that draw.
While the game plays, the wheel, a trackpad's fraction included, reaches its
UI, and the undrawn spot, which shows no marker then, picks nothing. Stopped,
it loads 300 undrawn spots and checks that the Scene shows a marker for each.
It then narrows the Scene to 240 dp beside the Text Editor and checks that the
rotate tool's hint and its Add button stay inside it, and that after the Move
tool takes over the hint fades out with its text. With the box and spot loaded
again, a marquee dragged through queued SDL events from below the box over the
spot marker's edge, short of its center, selects the spot. It saves no file.

`studio-acceptance --scene-cursors` needs no project or renderer. It checks the
cursor each Scene gizmo part asks for, a resize handle's as its box turns.

`studio-acceptance --graph-visibility --project-root PATH` takes a disposable
copy and runs ten hidden fixed-dt frames. It checks that maximizing Code and
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
frames. Pointer hit-testing and the Undo toast's button are not
exercised.

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
copy and runs at most eighteen hidden frames with reduced motion on, so toasts sit
where layout puts them. Counter's clean tab closes without asking. With Counter
edited, closing its tab asks "Save Counter before closing?" with Save and
Discard, and a click on Discard closes the tab without writing. Edited and asked
again, a Save by hand writes Counter, dismisses the prompt and keeps the tab
open. Then it adds an object to the start scene from a prototype only that run
registers and plays the scene. With Counter edited while it runs, Quit asks to
save both; a click on Save writes Counter, whose text applies live, and asks
again about the scene the running scene keeps from saving, naming how to save
it. A click on Discard then quits without writing the scene. The clicks are
pointer motion, press and release queued through SDL over the toast's buttons. The initial Counter bytes are
restored afterwards.

Execution requires explicit root coordination and independent admission under the
60-second GPU policy. Merely building this target does not run it. A whole UI
suite is not implied. The vendored pico_unit header preserves its upstream license
and original engine vendoring record; it is not installed with the Studio product.
