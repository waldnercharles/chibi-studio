# Live Outliner checkpoint

This branch is the user-requested checkpoint of paused work for continuation in
another workspace. Implementation remains paused. Remaining validation and
dependency integration are required before landing.

Use the matching engine checkpoint:

- Repository: `waldnercharles/chibi`
- Branch: `integration/outliner-m3-20261018`
- Commit: `e6d4c6dde6551d97bc9348220ff431d9a3b5186f`
- [Complete handoff, remaining checks and ownership constraints](https://github.com/waldnercharles/chibi/blob/e6d4c6dde6551d97bc9348220ff431d9a3b5186f/docs/live-outliner-handoff.md)
- [Portable reviews, exact source manifests and preserved test evidence](https://github.com/waldnercharles/chibi/tree/e6d4c6dde6551d97bc9348220ff431d9a3b5186f/docs/research/outliner-checkpoint-20261018)

Build and install that engine SDK, then explicitly configure Studio against the
installed package. Do not assume a previous build directory or SDK is present.
The original source base was `95ca95bc3ca2da5b899ec06596982ade26d2d94f`;
the newer, separately owned Studio PR71 work is not included in this checkpoint.
Coordinate with its owner before integrating it. The toolchain dependency is
published M3, not M4.

The live Outliner now uses owned capture, authored `.chibi` projection and
guarded model publication. The fixed 4,096-actor mounted test passes on bytecode
and SLJIT, including preservation of the correct model on execution exhaustion
before and after publication. Arithmetic, twelve live scenarios, guards,
projection, helpers, scale and a two-frame sanitizer startup also passed.
The final two fixture-header edits only corrected budget comments.

Worker-Wasm completion, final projects/tooling and installed-consumer checks,
web/browser validation and the retained rendered move checks remain pending.
The full native/render/Studio suites remain quarantined; keep the production
60-second test supervisor and existing fixture bounds. This checkpoint push
does not claim passing CI, merge approval, deployment or artifact publication.
