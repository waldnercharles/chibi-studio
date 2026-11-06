# Authored Studio acceptance

Studio uses the installed SDK's generic author host and UI replay protocol.
There is no native Studio test target. Fixtures dispatch ordinary pointer,
keyboard and text input through the application and may assert UI visibility,
authored data and source state. See the engine's `docs/testing/ui-replay.md`.

`studio_home_replay.json` is bounded to 24 fixed frames, eight input events and
six assertions. It opens New Project from Home, edits the name, checks the
visible form, cancels and returns to Home. Its presence is not a passing result.
Run it under the engine's admitted 60-second supervisor and save the host
receipt, SDK identity, source identity and capture together.

`studio_material_replay.json` is a separate bounded first measurement: 40 fixed
frames, 23 ordinary inputs and eight assertions. It switches to Output, creates
a Metal material with roughness 0.25 through the command palette and ordinary
fields, and opens its Surface preview. Run it only through the engine's
`studio-authored-material` admission with a pinned disposable workspace.
The canonical created source and the final PNG require separate inspection;
`visualValid` does not establish rendered pixels. This fixture does not cover
parameter undo/redo, evidence export or browser behavior.

Current SDK/source identities and scoped results are maintained once in the
engine's [canonical migration evidence](../../../chibi/docs/studio-rml-migration.md#current-candidate-evidence-2026-10-14).
Home acceptance for an earlier source revision does not validate the latest
source; Material40 and complete browser interaction remain unpassed until their
own receipts and captures are reviewed.

Repeatable admissions are being finalized in the engine checkout:

```sh
python3 tools/test_policy.py tool studio-authored-home
python3 tools/test_policy.py tool studio-authored-material
```

These normal-build routes are pending validation. Optional
`CHIBI_STUDIO_BUILD_DIR` and `CHIBI_STUDIO_SOURCE` overrides select the build and
source roots. Fresh disposable fixtures and evidence belong under
`build/test/studio`; follow the produced receipt rather than reusing an earlier
application or capture.

Keep source compilation, CPU service tests, rendered interaction and browser
acceptance separate. Broad quarantined suites must not be recreated as chunks.
The retired native routes remain described in [HISTORICAL_NATIVE.md](HISTORICAL_NATIVE.md).
The held fixture corpus has its own historical provenance; it is not current
executable coverage.
