#!/usr/bin/env bash
# Minimal-evidence host: raw engine output stays on the disposable runner.
set +x
set -euo pipefail
umask 077
WORK="${RUNNER_TEMP:?}/chibi-m5-private"
PUBLIC="$RUNNER_TEMP/chibi-m5-sanitized"
mkdir -p "$WORK" "$PUBLIC"
exec 3>&1
exec >>"$WORK/bootstrap.log" 2>&1
CONTROL="${GITHUB_WORKSPACE:?}/validation"
ROOT="$GITHUB_WORKSPACE/chibi"
BUILD="$WORK/build-language"
DEPS="$WORK/dependencies"
ADMISSION="$CONTROL/.github/toolchain-m5-admission.json"
TEST_SELECTOR=language-type-relations-chibi
ulimit -c 0

admission_valid() {
    [[ "${TYPE_RELATIONS_REVIEWED:-false}" == true ]] || return 1
    [[ "${CONTROL_SHA:-}" =~ ^[0-9a-f]{40}$ && "${CANDIDATE_SHA:-}" =~ ^[0-9a-f]{40}$ &&
       "${CANDIDATE_TREE:-}" =~ ^[0-9a-f]{40}$ ]] || return 1
    jq -e --arg repository "$GITHUB_REPOSITORY" --arg source "$CANDIDATE_SHA" \
      --arg tree "$CANDIDATE_TREE" --arg source_branch "$SOURCE_BRANCH" \
      --arg base "$PR_BASE_SHA" --arg base_branch "$PR_BASE_BRANCH" --arg branch "$PR_HEAD_BRANCH" '
      .schema == 2 and .minimal_evidence == true and .expected_result == "green" and
      .host_repository == "waldnercharles/chibi-studio" and .host_repository == $repository and
      .source_sha == $source and .source_tree == $tree and .source_branch == $source_branch and
      (.source_branch | test("^refs/heads/codex/m5-type-relations-[0-9]{8}$")) and
      .control_base_sha == $base and (.control_base_sha | test("^[0-9a-f]{40}$")) and
      .control_base_branch == "main" and .control_base_branch == $base_branch and
      .control_branch == "codex/host-m5-unit-validation-20261018" and .control_branch == $branch and
      (.source_review_sha256 | test("^[0-9a-f]{64}$")) and
      (.route_review_sha256 | test("^[0-9a-f]{64}$")) and
      .test_ceiling_seconds == 60 and .build_parallelism == 2
    ' "$ADMISSION" >/dev/null || return 1
}

checkout_identity() {
    local directory=$1 expected_head=$2 expected_tree=$3 head tree status
    head=$(git -C "$directory" rev-parse HEAD) || return 1
    [[ "$head" == "$expected_head" ]] || return 1
    tree=$(git -C "$directory" rev-parse 'HEAD^{tree}') || return 1
    [[ "$tree" =~ ^[0-9a-f]{40}$ ]] || return 1
    [[ -z "$expected_tree" || "$tree" == "$expected_tree" ]] || return 1
    # A failed git-status command with empty stdout is not a clean checkout.
    status=$(git -C "$directory" status --porcelain) || return 1
    [[ -z "$status" ]] || return 1
}

identity() {
    admission_valid || return 1
    checkout_identity "$CONTROL" "$CONTROL_SHA" '' || return 1
    checkout_identity "$ROOT" "$CANDIDATE_SHA" "$CANDIDATE_TREE" || return 1
}

prepare() {
    identity || return 1
    git -C "$CONTROL" rev-parse 'HEAD^{tree}' > "$WORK/control-tree-sha"
}

fetch_dependency() {
    local name=$1 url=$2 sha=$3
    git init "$DEPS/$name"
    git -C "$DEPS/$name" remote add origin "$url"
    git -C "$DEPS/$name" fetch --depth=1 origin "$sha"
    git -C "$DEPS/$name" -c advice.detachedHead=false checkout --detach FETCH_HEAD
    [[ "$(git -C "$DEPS/$name" rev-parse HEAD)" == "$sha" ]]
}

dependencies() {
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends clang-18 python3-venv git make ca-certificates jq
    python3 -m venv "$WORK/build-tools"
    "$WORK/build-tools/bin/pip" install --only-binary=:all: --require-hashes -r "$ROOT/.github/build-tools.txt"
    printf '%s\n' "$WORK/build-tools/bin" >> "$GITHUB_PATH"
    mkdir -p "$DEPS"
    fetch_dependency tree-sitter https://github.com/tree-sitter/tree-sitter.git da6fe9beb4f7f67beb75914ca8e0d48ae48d6406
    fetch_dependency css https://github.com/tree-sitter/tree-sitter-css.git dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f
    fetch_dependency html https://github.com/tree-sitter/tree-sitter-html.git 5a5ca8551a179998360b4a4ca2c0f366a35acc03
    fetch_dependency sljit https://github.com/zherczeg/sljit.git a0020dd49a21020434ff838fc308626c48cd8f05
    fetch_dependency cute https://github.com/RandyGaul/cute_framework.git 96e6034aa6bc44665856ee480962a9ddab0b87cc
}

configure() {
    cmake -S "$ROOT/test/language" -B "$BUILD" -G Ninja \
      -DCMAKE_C_COMPILER=clang-18 -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
      -DCHIBI_SDK_BUILD_COMPILER=ON -DCHIBI_SDK_BUILD_NATIVE=OFF \
      -DCHIBI_SDK_BUILD_DESKTOP=OFF -DCHIBI_ENABLE_SJLIT=ON \
      -DFETCHCONTENT_SOURCE_DIR_CHIBI_LANGUAGE_TREE_SITTER="$DEPS/tree-sitter" \
      -DFETCHCONTENT_SOURCE_DIR_CHIBI_UI_CSS="$DEPS/css" \
      -DFETCHCONTENT_SOURCE_DIR_CHIBI_UI_HTML="$DEPS/html" \
      -DCHIBI_SJLIT_SOURCE_DIR="$DEPS/sljit" \
      -DCHIBI_CUTE_MATH_INCLUDE_DIR="$DEPS/cute/include"
}

build() {
    cmake --build "$BUILD" --target test_language --parallel 2
    sha256sum "$BUILD/test_language" > "$WORK/binary.sha256"
}

execute() {
    [[ "${SELECTOR:-}" == "$TEST_SELECTOR" ]] || return 2
    identity || return 1
    # This route permits one full invocation, without subsets or retries.
    mkdir "$WORK/execution-started" || return 1
    sha256sum -c "$WORK/binary.sha256" || return 1
    printf '%s\n' "$CANDIDATE_SHA" > "$WORK/identity-$TEST_SELECTOR.verified"
    cd "$ROOT"
    CHIBI_LANGUAGE_BUILD_DIR="$BUILD" python3 tools/test_policy.py tool "$TEST_SELECTOR"
}

classify() {
    local outcome=not_started rc summary summary_count supervisor_count
    local log="$WORK/execute-$TEST_SELECTOR.log"
    local identity_verified=false summary_verified=false supervisor=false
    if identity && [[ -f "$WORK/identity-$TEST_SELECTOR.verified" ]] &&
       [[ "$(cat "$WORK/identity-$TEST_SELECTOR.verified")" == "$CANDIDATE_SHA" ]]; then
        identity_verified=true
    fi
    if [[ -f "$WORK/execute-$TEST_SELECTOR.exit" && -r "$log" ]]; then
        rc=$(cat "$WORK/execute-$TEST_SELECTOR.exit")
        summary_count=$(grep -c '^Summary:' "$log" || :)
        summary=$(grep -E '^Summary: Passed: 4 Failed: 0 Total: 4 Suites: [0-9]{1,12} Asserts: [1-9][0-9]{0,11}$' "$log" || :)
        if [[ "$summary_count" == 1 && -n "$summary" ]]; then summary_verified=true; fi
        supervisor_count=$(grep -c '^Test command wall time:' "$log" || :)
        if [[ "$supervisor_count" == 1 ]] &&
           grep -Eq '^Test command wall time: [0-9]+\.[0-9]{3}s; ceiling 60s; includes in-test fixture setup; external configuration/build excluded\.$' "$log" &&
           ! grep -Eq '^FAIL:|AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$log"; then
            supervisor=true
        fi
        case "$rc" in
          0)
            outcome=unexpected_test_result
            if [[ "$identity_verified:$summary_verified:$supervisor" == true:true:true ]]; then outcome=passed; fi ;;
          124) outcome=supervisor_timeout ;;
          1) outcome=unexpected_test_failure ;;
          *) outcome=runtime_or_supervisor_failure ;;
        esac
    fi
    [[ "$identity_verified" == true ]] || outcome=identity_unverified
    jq -n --arg selector "$TEST_SELECTOR" --arg outcome "$outcome" \
      --argjson identity "$identity_verified" --argjson summary "$summary_verified" --argjson supervisor "$supervisor" \
      '{selector:$selector,outcome:$outcome,execution_identity_verified:$identity,
        expected_pico_summary_verified:$summary,production_supervisor_60s_report_verified:$supervisor}' \
      > "$WORK/result-$TEST_SELECTOR.json"
}

collect() {
    local phase rc elapsed control_tree
    if ! admission_valid; then
        printf '{"outcome":"admission_unverified"}\n' > "$PUBLIC/sanitized.json"
        (cd "$PUBLIC" && sha256sum sanitized.json > SHA256SUMS)
        return
    fi
    classify
    : > "$WORK/phases.jsonl"
    for phase in prepare dependencies configure build "execute-$TEST_SELECTOR"; do
        [[ -f "$WORK/$phase.exit" && -f "$WORK/$phase.seconds" ]] || continue
        rc=$(cat "$WORK/$phase.exit"); elapsed=$(cat "$WORK/$phase.seconds")
        [[ "$rc" =~ ^(0|[1-9][0-9]{0,2})$ && "$elapsed" =~ ^(0|[1-9][0-9]{0,7})$ ]] || return 1
        jq -n --arg phase "$phase" --argjson exit "$rc" --argjson seconds "$elapsed" \
          '{phase:$phase,exit:$exit,elapsed_seconds:$seconds}' >> "$WORK/phases.jsonl"
    done
    control_tree=$(cat "$WORK/control-tree-sha") || return 1
    [[ "$control_tree" =~ ^[0-9a-f]{40}$ ]] || return 1
    jq -s '.' "$WORK/phases.jsonl" > "$WORK/phases.json"
    jq -n --arg source_sha "$CANDIDATE_SHA" --arg source_tree "$CANDIDATE_TREE" \
      --arg control_sha "$CONTROL_SHA" --arg control_tree "$control_tree" \
      --slurpfile results "$WORK/result-$TEST_SELECTOR.json" --slurpfile phases "$WORK/phases.json" \
      '{schema:1,identity:{source_sha:$source_sha,source_tree:$source_tree,control_sha:$control_sha,control_tree:$control_tree},
        phases:$phases[0],results:$results,raw_logs:"runner_local_discarded",test_ceiling_seconds:60}' > "$PUBLIC/sanitized.json"
    (cd "$PUBLIC" && sha256sum sanitized.json > SHA256SUMS)
}

verdict() {
    local control_tree
    identity || return 1
    [[ -f "$WORK/collect.exit" && "$(cat "$WORK/collect.exit")" == 0 ]] || return 1
    control_tree=$(git -C "$CONTROL" rev-parse 'HEAD^{tree}') || return 1
    jq -e --arg source "$CANDIDATE_SHA" --arg tree "$CANDIDATE_TREE" \
      --arg control "$CONTROL_SHA" --arg control_tree "$control_tree" '
      .schema == 1 and .test_ceiling_seconds == 60 and
      .identity.source_sha == $source and .identity.source_tree == $tree and .identity.control_sha == $control and
      .identity.control_tree == $control_tree and
      ([.phases[].phase] == ["prepare","dependencies","configure","build","execute-language-type-relations-chibi"]) and
      all(.phases[]; .exit == 0 and (.elapsed_seconds | type == "number" and . >= 0)) and
      (.results | length == 1) and .results[0].selector == "language-type-relations-chibi" and
      .results[0].outcome == "passed" and .results[0].execution_identity_verified == true and
      .results[0].expected_pico_summary_verified == true and .results[0].production_supervisor_60s_report_verified == true
    ' "$PUBLIC/sanitized.json" >/dev/null
}

phase=${1:-invalid}
case "$phase" in prepare|dependencies|configure|build|execute|collect|verdict) ;; *) printf 'phase=invalid exit=2\n' >&3; exit 2 ;; esac
key=$phase
if [[ "$phase" == execute ]]; then
    [[ "${SELECTOR:-}" == "$TEST_SELECTOR" ]] || { printf 'phase=execute exit=2\n' >&3; exit 2; }
    key="execute-$TEST_SELECTOR"
fi
started=$SECONDS
set +e
(set -euo pipefail; "$phase") 3>&- >"$WORK/$key.log" 2>&1
rc=$?
set -e
elapsed=$((SECONDS-started))
printf '%s\n' "$rc" > "$WORK/$key.exit"
printf '%s\n' "$elapsed" > "$WORK/$key.seconds"
printf 'phase=%s exit=%d elapsed_seconds=%d\n' "$key" "$rc" "$elapsed" >&3
exit "$rc"
