The optional native `studio-acceptance` target compiles the actual application and
host in test wrappers. The production binary has no fixture CLI or test hooks.
Build with `-DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON` against an installed SDK.

`studio-acceptance --source-edit --project-root PATH` accepts a disposable copy of
this directory's `project`. Preferences and thumbnail cache are pinned inside that
disposable project; initial Counter bytes are checked before and after. It opens Counter through the normal document loader,
then runs four hidden fixed-dt frames. It calls the production graph expression
commit handler once, applies real undo and redo, and checks current source,
resolved state initializer, owned projection and dirty/history state. With
nothing unsaved, quitting needs no confirmation; with Counter unsaved, the window
title marks it and a quit request asks once, in a toast a repeated request updates.
The final frame renders both graph and code views before normal host shutdown. This proves
consumer edit-handler integration and UI rendering; it does not hit-test pointer
input. No source save, scenario, playback or screenshot occurs.

`studio-acceptance --scene-cursors` needs no project or renderer. It checks the
cursor each Scene gizmo part asks for, a resize handle's as its box turns.

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
