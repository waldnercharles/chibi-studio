# Historical syntax fixtures

These five files came from the small `prototypes/code-language` frontend
experiment: projectile, recharge, cooldown, branching, and expressions.
The experiment parsed and checked a demo catalog containing self, destroy,
clamp, and Tick. Its implementation and build targets have been retired.

The production syntax corpus retains these inputs to cover state, branches,
operators, multiline expressions, and calls. Passing their syntax check does
not establish production catalog or execution compatibility. Executable SDK
examples and their runtime proof are in `examples/language` and
`test/language/program.c`.
