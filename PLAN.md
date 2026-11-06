# Texture asset lifecycle

The completed slice adds Assets entries and previews, a Properties
inspector with persistent source/import settings, explicit reimport at the same
image key, and drag into a material. `.chibi` handles inspector events and view
state; SDK APIs validate and publish files; the existing `asset_code` history
handles settings edits. Reuse the Assets thumbnail cache and graph drag/history.

Settings save independently from pixel reimport. Reimport saves pending settings,
then validates source, target and budget before replacing the derived image.
Material nodes retain their sampling settings; new dropped nodes use the texture
semantic to choose sRGB or linear. Extend the existing bounded native selector
for settings undo/redo, reimport, graph drag undo/redo and fresh-process reopen.
Check web rendering separately. No new tools or editor document types.

Verified locally on 2026-10-16 with the extended existing native selector:
46 parent frames plus eight fresh-process frames, 8.432 seconds total under the
60-second supervisor. It checks the Assets preview, settings edit/undo/redo/save,
one-step drag creation/undo/redo, image swapping, same-key reimport after source
pixels change, unchanged material references, and settings/pixels after reopen.
The original Grey Rock source remains unchanged. Native and web builds consume
the installed SDK. A bounded browser check at 1280×720 verifies the preview and
settings with fixed Save settings/Reimport actions above the scrolling body.

The inspector behavior is in `TextureAsset.chibi`; shared native services own
documents, previews and SDK publication. Existing images without import metadata
can be browsed and dragged, but cannot be reimported through this inspector.
Source relinking, browser/Windows publication and texture rename/delete remain
outside this slice. Reimport changes pixels on disk; settings and material edits
use Undo. No new CLI commands or selectors were added. Work remains uncommitted.

# Grey Rock texture workflow checkpoint

Implement one existing-host workflow while keeping interaction state in authored Studio sources.

- Repair string image choices and persist virtual `content/...` references through existing material graph edits/history.
- Add a bounded shared Studio service operation using the installed SDK image import plan/commit APIs. The native adapter owns OS callbacks and immutable plans; `.chibi` owns options, review, confirmation and feedback.
- Reuse the Assets header popover, existing fields and searchable graph choices. Offer aspect-preserving 1K, 2K and supported source dimensions, plus color/scalar/normal filtering semantics. Never replace source or destination files.
- Report resident visual payload measurements explicitly. Scene resources and parameter records are excluded from this live measurement. Complete saved-project compiler/export admission remains authoritative.
- Add a separately admitted bounded UI selector using real SDL input and injected source selection only at the OS dialog boundary. Verify import, image replacement, undo/redo, save/reopen, virtual reference preservation and a meaningful rendered material. Root coordinates exact SDK packaging, review and supervised execution.

Source compilation, bounded native interaction, browser interaction and terrain persistence are separate evidence. This checkpoint does not import the broad authored-host migration or establish complete browser acceptance.

Completed locally on 2026-10-16. The native selector passes 32 frames across two
fresh host processes, using Grey Rock01 base-color and roughness controls. It
verifies import, swap, undo/redo, save and resident pixels after reopen. The final
run took 5.118 seconds under the production 60-second supervisor. Review and scene
captures were visually inspected. Native and serial web builds consume the
installed SDK from the sibling engine candidate.

Browser checks verified the import popup's placement and styled fields at
1280×720. They also found a shared WebGL texture-unit overflow, repaired in the
engine's HRC composite. Native POSIX import is implemented; browser/Windows file
publication and 16-bit PNG conversion remain explicit limitations. An existing
browser storage-sync warning remains. Engine terrain persistence checks pass
separately. See `../chibi/docs/grey-rock-checkpoint.md` for identities and results.

The candidate remains uncommitted and unmerged. Voidweaver retains game visuals.

The import dialog was simplified later on 2026-10-16: filename-first source
selection, grouped options, a compact size/budget summary, and one primary action
for Review, Import, or Done. Completion state remains authored in `.chibi`.
The existing 32-frame import/history/reopen check was reused; no engine code,
SDK contract, test selector, or tooling registration was added for this redesign.
Native and web builds pass. Browser checks at 1280×720 exercised keyboard size
selection and Cancel; native checks cover actual import and Done. Browser file
import remains unsupported. Formatting used a disposable migrated Studio copy,
which was removed afterward.
