# Outliner request checkpoint allowance

The production coefficient is **K = 249**, the sum of canonical `BF_CHECK`
sites in the exact shipped closure below. It was independently qualified
against the integrated request-fuel ABI on 2026-10-18. The preserved
[census.log](census.log) reports one passing C/pico case, 393 assertions,
22 compile-time constant evaluations, and no runtime controller/helper calls.
Its whole-invocation wall time was 0.475 seconds under the unchanged 60-second
production supervisor. Compilation and runtime accounting are separate checks.

These identities are evidence for the workload bound. They do not authorize
publication, reject safe authored edits, or replace the prepared-producer and
current-request authority checks. Safe edited programs receive a finite
allowance and can exhaust it normally. The source hashes are not an allowlist.

## Qualified inputs

All identities below are SHA-256. Studio and engine paths are relative to their
respective repositories; the manifest paths are engine-relative.

| Input | SHA-256 |
| --- | --- |
| Studio `projects/studio/content/behaviors/OutlinerRows.chibi`, 1441 bytes | `510f2155a49ff77724efd38be5d1098f5ac5957c62acb0c06eb755de22e8a450` |
| Studio `projects/studio/content/libraries/OutlinerRowPolicies.chibi`, 11668 bytes | `e2c727396b2a5004327fcf17cd3e1a8194629c928db75c4927ef6f4cae9c28a5` |
| Engine `src/language/lower.c` | `2658fc232b983e2d91a983998e3ad5e306963a428f9687e11c23ca83d778cc97` |
| Engine `src/execution/function.h` | `fe1e9b69f923f7169f7731e78adecb7325d03b1a98dc12a60ce58b84bc50e09c` |
| Engine `src/execution/prepared.c` | `cfec73fa439511c5d7fa368ad646e1b35cd0d01ed5ed74c56c57fc85924512a8` |
| Engine `src/execution/row_builder.c` | `e5f379006f46b3df4d15f97e4aa5396b73dcc65926878b803a4fcdfb605eb520` |
| Engine `test/language/outliner_work.c` | `9c2af9b1ab45cf5aae5a8d9cda33fbf6856fdee3acff9164267c5d7409aa03b2` |
| Preserved [engine-fuel.sha256](engine-fuel.sha256), all 14 entries verified at census | `95af69dc75b23505bae0ebec0d31c3860f47aa8e1c6a8f269c32bf5be7b3a8b1` |
| Preserved [census.log](census.log) | `530ceb49ea65144694a986b57b26829d430725c24dab3f7663617388f80ecc70` |

The census prepares one bytecode collection from seven fixed overlays using
the Outliner, Array and Text native definitions, three exact native Outliner
records and the entity nominal type. It verifies all 20 body declarations and
the actual imported module pointer, then follows actual synchronous callees
from Refresh and RevealObject. Unknown bodies/tasks, an incomplete closure,
and nonzero prepaid `root_steps` are rejected. It reads canonical bodies,
never optimized runtime instruction counts or an execution-cost summary.

The context ABI contains a borrowed `execution_work *work` and a two-uintptr_t
spent/limit balance. The prepared fingerprint includes the balance size and
offsets and `behavior_function_context.work`. Configured public/private context
headers matched `function.h` above. This qualifies the census against that ABI
implementation; the fixture does not print a numeric wire ABI fingerprint or
perform a prepared-image serialization round trip.

## Closure and conservative formula

| Reachable body | Canonical checkpoints |
| --- | ---: |
| OutlinerRows.Refresh | 6 |
| OutlinerRows.RevealObject | 7 |
| Depth | 9 |
| Icon | 17 |
| Drop | 49 |
| SelectState | 4 |
| FoldByte | 7 |
| ContainsFolded | 21 |
| LastIndex | 21 |
| Title | 13 |
| IntRange | 9 |
| BehaviorCount | 12 |
| Page | 21 |
| Project | 53 |
| **14 bodies, 5035 canonical instructions** | **249** |

Both request handlers are included conservatively although only one executes
per request. The six other handlers are excluded. Project's two scalar append
plans and IntRange's one plan preserve canonical checkpoint charging.

The checked native metadata derives the following from the exact owned capture:
N actors, A raw attachment entries, path byte length l, viewport upper bound V,
maximum ancestor depth h, maximum prototype component span cmax, and maximum
actor field span fmax. The field spans are disjoint with total F. For each actor
name and prototype name of byte length n and filter length q, let
`T(n,q) = n-q+1` when `0 < q <= n`, otherwise zero, and `R(n,q) = q*T(n,q)`.
Sum T and R over both strings of every actor. Define
`D=V*h`, `Cv=V*cmax`, `Fv=min(F,V*fmax)`, and `Av=V*A`.

```
W = 8 + 3*N + 8*V + T + 3*R + D + 2*Cv + 2*Fv + Av + l + min(l,127)
absolute request allowance = 10000 + 249*W
```

V matches the shipped Page upper bound with shown=N: clamp
`First>0 ? First-1 : 0` to N; Count<=0 gives zero; otherwise reserve one slot
when First<1, then clamp to remaining actors. Collapsed gives V=0. Comparisons
precede subtraction/conversion, including INT_MIN and INT_MAX. Reveal's
pre-Capture forced-open state is used. No new scene-size ceiling is introduced.

W sums a bound on invocations plus loop iterations across the closure: one
handler; Project `1+N+V`; Page 1; ContainsFolded `2N+T+R`; FoldByte `2R`;
Depth `V+D`; Icon `V+Cv`; Drop `V+Fv`; IntRange `2V+Cv+Fv`; SelectState V;
BehaviorCount `V+Av`; Title 1; LastIndex `4+l+min(l,127)`. A static checkpoint
can execute at most once per loop-free invocation path plus once per relevant
iteration. Multiplying total visits by the sum of all static checkpoint sites
therefore overcounts safely for this frozen acyclic shipped algorithm. The
factor 3R includes the nested comparison and both FoldByte calls; repeated
prototype spans use V*cmax rather than total captured components.

Every intermediate operation is checked in uintptr_t, including V*fmax before
the minimum and the final K multiplication/bootstrap addition. Overflow fails
before Project/Publish. A successful exact Capture grants that same request's
absolute total once, preserving prior spend. Refresh/Reveal alone opt in;
ordinary execution retains its existing budget. Capture, Project, Publish and
the post-Publish tail remain synchronous and charged. Failure preserves the
last successful valid publication, including an accepted Publish before a tail
error. Native callbacks are neither interrupted nor fully metered by authored
checkpoints; this is no latency or prompt-cancellation guarantee.

## Historical measurement and reproduction

The original artifact root was `/workspace/outliner-build`. The independently
reviewed qualification is `census-final-abi-review.md`, SHA-256
`29e04a2eafc5dc0786dc5ac64ad74d7176c801dacb04517b8856864d0a52a8e5`.
Its historical provenance includes:

| Artifact | SHA-256 |
| --- | --- |
| `native/test/language/test_language` observed after the census | `6b1602cb347ccc3bc44581b4b854a6a8b57cce2960a994bb5999dac90d881aa6` |
| `native-build-22.log` | `87ba0ee6fc7f1a6f21050482a26709390d195656c2b1fc66a40c9041c3a2a8ab` |
| `native/compile_commands.json` | `6238345ac3b77e3f37a903df559d451e2a73f5d9b8417ad527d6ae9cc9d12d90` |
| `native/CMakeCache.txt` | `9b607a480d7b08bc5441e777c1d505841c64a3d9454cb645534eb0fa5c48221f` |

Compilation records identify Clang 19, Debug, ASan+UBSan, frame pointers and
`-UNDEBUG` for the fixture. The build completed before this measured census.
The human build label alone is not artifact identity. No clean final engine/
Studio tree IDs or final installed SDK build/source-snapshot identity existed
at draft qualification; final release validation must record them separately.
Do not treat this evidence as a final installed-consumer or runtime pass.

To reproduce from the engine checkout, build the C language driver separately,
then use its existing standalone route under the production supervisor:

```sh
CHIBI_LANGUAGE_BUILD_DIR=<absolute-reviewed-language-driver-directory> \
CHIBI_OUTLINER_CONTENT=<absolute-Studio-project-content-directory> \
python3 tools/test_policy.py tool language-outliner-work
```

This is a parameterized reproduction command, not a claimed transcript of the
historical environment. Preserve actual invocation and build records for the
new run. Requalify changed sources, catalog/preparation, lowering/checkpoint
placement or context/prepared ABI against these inputs. An unrelated fixture
correction does not change K; runtime debit changes still need their own
execution review/tests. Engine accounting/backend, Studio arithmetic/live,
Wasm and final installed-consumer checks remain independently required.
