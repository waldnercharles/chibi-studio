# Browser storage regression tests

Run `node --test tests/web/*.test.cjs` from Studio on Linux or macOS. CI uses Node 22.
The browser-export isolation case also needs a C compiler with `-std=c2x`,
selected by `CC` or `cc`. No engine checkout, SDK or SDL headers are needed.
Children retain the old fixture's ten-second deadline and POSIX group cleanup.
Two supervision controls cover ignored TERM and a completed leader with live
descendants; the 19 browser-storage/export cases remain separate.

These are the 19 cases formerly dispatched by the engine's
`test/tools/web_storage.c` at `813b93875`. Eighteen run Studio's actual
`web_storage.js` and the script from `web_shell.html` in an isolated Node VM.
The in-memory filesystem, flush completion, fetch responses, browser events and
timers preserve the original fixture seams. Each scenario starts in its own
process and retains the original assertions on startup, integrity failures,
workspace preservation, persistence races and retry behavior.

The nineteenth compiles the actual `__EMSCRIPTEN__` branch of
`studio_export.c` against its opaque Studio type. This keeps native process APIs
out of the browser export branch without a C test executable.

The old embedded dispatcher had two invalid JavaScript declarations,
`async launch :: fn` and `async startImport :: fn`. Node rejected it before
running a storage scenario. The moved driver uses ordinary async functions;
the production browser scripts are unchanged. These tests exercise browser/FS
seams under Node, rather than an actual IndexedDB implementation in a browser.
