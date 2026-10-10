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

`studio-acceptance --export-game --project-root PATH` accepts another disposable
copy. Its first hidden frame runs the Export Game command, which needs the SDK's
`chibi`, `chibi-compile` and `chibi-player` that the build copies beside it. The
second frame waits for the exporter to exit, and the third frame's own poll
reports it. It checks that the command is disabled while the export runs, that
Studio reports the export, and that the reported folder holds the compiled game.
Showing the folder is not exercised.

Execution requires explicit root coordination and independent admission under the
60-second GPU policy. Merely building this target does not run it. A whole UI
suite is not implied. The vendored pico_unit header preserves its upstream license
and original engine vendoring record; it is not installed with the Studio product.
