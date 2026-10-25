# PR77 browser preview

The preview is limited to `/previews/77/`. Its application source is frozen at
`c28301748699a158fec08a475ca7dde872bb4960`, tree
`9cfd1b700234e09df5f5611792e94e36e08127da`, against engine
`d1154cdb8677cbfe479d4be40db7ca480d10f103`. Changing that candidate requires another
reviewed source change. The workflow uses the released serial SDK and Emscripten
6.0.10 on CI. It installs no toolchain on the operator's Mac.

After this infrastructure is reviewed and merged, dispatch the trusted workflow
on `main` with the preview input:

```sh
gh workflow run publish.yml --ref main -f preview_77=true
```

The application checkout is separate from the publishing tools. Packaging checks
the actual clean source commit/tree and SDK identity, and records those identities
and file checksums in the preview's `studio.json`. PR checks build without uploading
an application or Pages artifact. The existing `github-pages` environment remains
restricted to `main`; deployment never executes the preview checkout's tools.

Pages deploys a complete site. Production updates, preview updates and cleanup
therefore share a publisher lock with `queue: max` and `cancel-in-progress: false`.
After obtaining that lock, the publisher restores the last successfully deployed
`github-pages` artifact from this repository's trusted main workflow and verifies
its digest. It preserves the production root and every other preview when replacing
`previews/77/`. Normal production updates preserve all previews. A stale production
build cannot replace the current main revision.

The publisher checks PR77's current state inside the lock. A closed or merged PR
cannot publish a preview. A metadata-only `pull_request_target: closed` event with
`merged == true` removes only `previews/77/` through the same publisher. It never
checks out PR code. An unmerged close does not remove the preview. This trigger does
not authorize merging PR77.

Complete site artifacts are retained for 90 days. Missing, expired, corrupt or
untrusted snapshots stop publication instead of reconstructing an incomplete site.
If the site has been idle beyond retention, a reviewed recovery of its complete
published contents is required before publishing again. Before the first deployment
of this infrastructure, let any older publishing workflow finish so it cannot later
replace the site using the previous assembly rules.

The preview uses a serial browser build. GitHub Pages does not inherit the local
server's COOP/COEP headers. Threaded isolation and its service-worker fallback are
separate acceptance work. Use a fresh browser profile for QA: paths on this site
share an origin, the `chibi.studio` localStorage key and IndexedDB workspace.

Run the SDK-free tooling fixtures locally:

```sh
cc -std=c11 -Wall -Wextra -Werror -Itests/acceptance tests/tools/main.c -o /tmp/studio-tools
/tmp/studio-tools --pages-site "$PWD"
rm /tmp/studio-tools
```

The C dispatcher exercises production preservation, preview replacement, interleaved
updates, merged-only cleanup, stale builds, corrupt bundles and archive path/link
rejection. It creates tiny disposable fixtures and runs no compiler SDK, browser,
network request, upload or deployment. Browser acceptance must identify the deployed
preview manifest and exercise physical node-field and source-caret input separately.
