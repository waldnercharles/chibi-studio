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
After obtaining that lock, the publisher reads the `github-pages` environment's
deployment/status history and selects the latest success by completion time. Its
exact job URL identifies the successful attempt even if a later rerun failed. The
trusted main job's deploy-pages log records the immutable artifact ID. The publisher
downloads only that artifact and verifies its digest, source and run identity. It
never selects a surviving upload as a substitute for a deleted deployed snapshot.
History reads stop after five pages of 20 records each or 100 API requests; an
incomplete history stops publication. It preserves the production root and every
other preview when replacing
`previews/77/`. Normal production updates preserve all previews. A stale production
build cannot replace the current main revision.

The publisher checks PR77's current state inside the lock. A closed or merged PR
cannot publish a preview. A metadata-only `pull_request_target: closed` event with
`merged == true` removes only `previews/77/` through the same publisher. It never
checks out PR code. An unmerged close does not remove the preview. This trigger does
not authorize merging PR77.

GitHub's `pull_request_target` run, deployment and artifact metadata name the PR
head even though its workflow executes on the default branch. Restoration admits
only the merged PR77 cleanup: its same-repository head must match those records,
its base must be main, and the run must follow the recorded merge. The successful
job's Pages build version must equal PR77's merge commit, while its exact artifact
must match the triggering run. Ordinary publications still require main metadata.
Unexpected identities stop restoration; they never select an older snapshot.

Complete site artifacts are retained for 90 days. Missing, expired, corrupt or
untrusted snapshots and unavailable deployment logs stop publication instead of
reconstructing an incomplete site. Deployment history requires read access to
deployments; the job declares that read-only token scope alongside Actions read.
Binary log/archive reads use `gh api --allow-escape-sequences` (GitHub CLI 2.97+
as supplied by the runner). The helper captures these bytes for parsing and checksum
verification; it never prints the remote log. JSON requests retain default protection.
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

The C dispatcher compares complete retained file maps using distinct before/incoming
content. It exercises production preservation, preview replacement, interleaved
updates, merged-only cleanup, stale builds, corrupt bundles and archive path/link
rejection. Offline API fixtures cover deleted latest snapshots, failed reruns,
newer failed uploads, pagination, untrusted provenance and missing logs. Package
fixtures check clean source/tree, serial SDK identity and recorded file checksums.
It creates tiny disposable fixtures and runs no compiler SDK, browser,
network request, upload or deployment. Browser acceptance must identify the deployed
preview manifest and exercise physical node-field and source-caret input separately.
