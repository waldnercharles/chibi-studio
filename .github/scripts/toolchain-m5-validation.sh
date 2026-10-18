#!/usr/bin/env bash
# Minimal-evidence host: raw engine output never reaches stdout or artifact paths.
set +x
set -euo pipefail
umask 077
WORK="${RUNNER_TEMP:?}/chibi-m5-private"
PUBLIC="$RUNNER_TEMP/chibi-m5-sanitized"
mkdir -p "$WORK" "$PUBLIC"
# Keep one explicit descriptor for fixed, allowlisted status lines only.
exec 3>&1
exec >>"$WORK/bootstrap.log" 2>&1
CONTROL="${GITHUB_WORKSPACE:?}/validation"
ROOT="$GITHUB_WORKSPACE/chibi"
BUILD="$WORK/build-language"
DEPS="$WORK/dependencies"
ADMISSION="$CONTROL/.github/toolchain-m5-admission.json"
ulimit -c 0

prepare() {
    [[ "$GITHUB_REPOSITORY" == "$(jq -r .host_repository "$ADMISSION")" ]]
    [[ "$CONTROL_SHA" =~ ^[0-9a-f]{40}$ && "$CANDIDATE_SHA" =~ ^[0-9a-f]{40}$ ]]
    [[ "$(git -C "$CONTROL" rev-parse HEAD)" == "$CONTROL_SHA" ]]
    [[ -z "$(git -C "$CONTROL" status --porcelain)" ]]
    [[ "$PR_BASE_SHA" == "$(jq -r .control_base_sha "$ADMISSION")" ]]
    [[ "$PR_BASE_BRANCH" == "$(jq -r .control_base_branch "$ADMISSION")" ]]
    [[ "$PR_HEAD_BRANCH" == "$(jq -r .control_branch "$ADMISSION")" ]]
    [[ "$CANDIDATE_SHA" == "$(jq -r .source_sha "$ADMISSION")" ]]
    [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$CANDIDATE_SHA" ]]
    [[ "$(git -C "$ROOT" rev-parse 'HEAD^{tree}')" == "$(jq -r .source_tree "$ADMISSION")" ]]
    [[ -z "$(git -C "$ROOT" status --porcelain)" ]]
    jq -e '
      .schema == 2 and .minimal_evidence == true and
      (.expected_result == "red" or .expected_result == "green") and
      (.source_sha | test("^[0-9a-f]{40}$")) and
      (.source_tree | test("^[0-9a-f]{40}$")) and
      (.source_review_sha256 | test("^[0-9a-f]{64}$")) and
      (.route_review_sha256 | test("^[0-9a-f]{64}$")) and
      .test_ceiling_seconds == 60 and .build_parallelism == 2
    ' "$ADMISSION" >/dev/null
    if [[ "$(jq -r .expected_result "$ADMISSION")" == green ]]; then
        jq -e '.baseline_evidence_sha256 | test("^[0-9a-f]{64}$")' "$ADMISSION" >/dev/null
        printf 'mode=green\n' >> "$GITHUB_OUTPUT"
    else
        printf 'mode=red\n' >> "$GITHUB_OUTPUT"
    fi
    # Only validated identity digests enter the later public projection.
    git -C "$ROOT" rev-parse 'HEAD^{tree}' > "$WORK/source-tree-sha"
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
    [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$CANDIDATE_SHA" ]]
    [[ -z "$(git -C "$ROOT" status --porcelain)" ]]
    local mode
    mode=$(jq -r .expected_result "$ADMISSION")
    case "$mode:$SELECTOR" in
      red:language-unit-analysis-zero-id|red:language-unit-analysis-deep|green:language-unit-analysis-chibi) ;;
      *) return 2 ;;
    esac
    sha256sum -c "$WORK/binary.sha256"
    printf 'true\n' > "$WORK/identity-$SELECTOR.verified"
    cd "$ROOT"
    # One unchanged production supervisor invocation per independently admitted
    # selector. No direct driver call, deadline override, retry or aggregate.
    CHIBI_LANGUAGE_BUILD_DIR="$BUILD" python3 tools/test_policy.py tool "$SELECTOR"
}

classify() {
    local selector=$1 outcome=$2 key log rc mode summary passed=-1 failed=-1 total=-1 suites=-1 asserts=-1
    local witness_bytecode=false witness_sljit=false zero_bytecode=false zero_sljit=false
    local depth_bytecode=false depth_compiled=false supervisor=false
    key="execute-$selector"
    log="$WORK/$key.log"
    mode=$(jq -r .expected_result "$ADMISSION")
    if [[ -f "$WORK/$key.exit" ]]; then
        rc=$(cat "$WORK/$key.exit")
        summary=$(grep -E '^Summary: Passed: [0-9]{1,12} Failed: [0-9]{1,12} Total: [0-9]{1,12} Suites: [0-9]{1,12} Asserts: [0-9]{1,12}$' "$log" || :)
        if [[ $(grep -c '^Summary:' "$log") == 1 && "$summary" =~ ^Summary:\ Passed:\ ([0-9]+)\ Failed:\ ([0-9]+)\ Total:\ ([0-9]+)\ Suites:\ ([0-9]+)\ Asserts:\ ([0-9]+)$ ]]; then
            passed=${BASH_REMATCH[1]}; failed=${BASH_REMATCH[2]}; total=${BASH_REMATCH[3]}
            suites=${BASH_REMATCH[4]}; asserts=${BASH_REMATCH[5]}
        fi
        if grep -Eq '^Test command wall time: [0-9.]+s; ceiling 60s;' "$log" &&
           ! grep -Eq '^FAIL:|AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' "$log"; then
            supervisor=true
        fi
        case "$rc" in
          0)
            outcome=unexpected_baseline_pass
            if [[ "$mode" == green ]]; then
                outcome=unexpected_test_result
                if [[ "$passed:$failed:$total:$supervisor" == 3:0:3:true && "$asserts" -gt 0 ]]; then outcome=passed; fi
            fi ;;
          124) outcome=supervisor_timeout ;;
          1) outcome=unexpected_test_failure ;;
          *) outcome=runtime_or_supervisor_failure ;;
        esac
        if [[ "$selector" == language-unit-analysis-zero-id ]]; then
            grep -Fxq 'unit analysis regression case 7 backend bytecode: C witness passed' "$log" && witness_bytecode=true
            grep -Fxq 'unit analysis regression case 7 backend SLJIT: C witness passed' "$log" && witness_sljit=true
            grep -Fxq 'unit analysis zero-ID observation: backend=bytecode diagnostics=0 result=unknown' "$log" && zero_bytecode=true
            grep -Fxq 'unit analysis zero-ID observation: backend=SLJIT diagnostics=0 result=unknown' "$log" && zero_sljit=true
            if [[ "$rc:$mode:$passed:$failed:$total:$supervisor:$witness_bytecode:$witness_sljit:$zero_bytecode:$zero_sljit" == 1:red:0:2:2:true:true:true:true:true ]] &&
               ! grep -q '^compiler fixture failure:' "$log"; then outcome=intended_assertion_failure; fi
        fi
        if [[ "$selector" == language-unit-analysis-deep ]]; then
            grep -Fxq 'unit analysis regression case 24 backend bytecode: C witness passed' "$log" && witness_bytecode=true
            grep -Fxq 'unit analysis regression case 24 backend SLJIT: C witness passed' "$log" && witness_sljit=true
            grep -Eq '^compiler fixture failure: backend=bytecode package=project:unit_analysis entry=Analyze status=[0-9-]+ returned=0 failure=depth category=1 error_steps=[0-9]+ message=' "$log" && depth_bytecode=true
            grep -Eq '^compiler fixture failure: backend=compiled package=project:unit_analysis entry=Analyze status=[0-9-]+ returned=0 failure=depth category=1 error_steps=[0-9]+ message=' "$log" && depth_compiled=true
            if [[ "$rc:$mode:$passed:$failed:$total:$supervisor:$witness_bytecode:$witness_sljit:$depth_bytecode:$depth_compiled" == 1:red:0:2:2:true:true:true:true:true ]] &&
               [[ $(grep -c '^compiler fixture failure:' "$log") == 2 ]]; then outcome=intended_assertion_failure; fi
        fi
    fi
    [[ -z "$(git -C "$ROOT" status --porcelain)" ]] || outcome=source_mutation
    [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$CANDIDATE_SHA" ]] || outcome=source_mutation
    # Publish only enum/boolean test status. Raw summaries, diagnostics and
    # binary/source-derived files stay private, even when a check fails.
    local identity_verified=false summary_verified=false regression_verified=false
    [[ ! -f "$WORK/identity-$selector.verified" ]] || identity_verified=true
    if [[ "$mode:$passed:$failed:$total" == red:0:2:2 || "$mode:$passed:$failed:$total" == green:3:0:3 ]]; then summary_verified=true; fi
    [[ "$outcome" != intended_assertion_failure ]] || regression_verified=true
    jq -n --arg selector "$selector" --arg outcome "$outcome" \
      --argjson identity "$identity_verified" --argjson summary "$summary_verified" \
      --argjson regression "$regression_verified" --argjson supervisor "$supervisor" \
      '{selector:$selector,outcome:$outcome,execution_identity_verified:$identity,
        expected_pico_summary_verified:$summary,expected_red_markers_verified:$regression,
        production_supervisor_60s_report_verified:$supervisor}' > "$WORK/result-$selector.json"

}

collect() {
    local status=not_started phase rc elapsed source_tree control_tree mode
    # A missing source checkout (including privacy-guard refusal) is a blocker.
    if [[ ! -d "$ROOT/.git" ]]; then
        printf '{"outcome":"private_checkout_not_performed"}\n' > "$PUBLIC/sanitized.json"
        (cd "$PUBLIC" && sha256sum sanitized.json > SHA256SUMS)
        return
    fi
    for phase in prepare dependencies configure build; do
        if [[ -f "$WORK/$phase.exit" ]] && [[ "$(cat "$WORK/$phase.exit")" != 0 ]]; then
            status="${phase}_failure"; [[ "$phase" != build ]] || status=compile_failure; break
        fi
    done
    mode=$(jq -r .expected_result "$ADMISSION")
    case "$mode" in
      red) classify language-unit-analysis-zero-id "$status"; classify language-unit-analysis-deep "$status" ;;
      green) classify language-unit-analysis-chibi "$status" ;;
      *) return 2 ;;
    esac
    : > "$WORK/phases.jsonl"
    # Only fixed phase names and numeric statuses/times leave.
    for phase in prepare dependencies configure build execute-language-unit-analysis-zero-id execute-language-unit-analysis-deep execute-language-unit-analysis-chibi; do
        [[ -f "$WORK/$phase.exit" ]] || continue
        rc=$(cat "$WORK/$phase.exit"); elapsed=$(cat "$WORK/$phase.seconds")
        [[ "$rc" =~ ^[0-9]{1,3}$ && "$elapsed" =~ ^[0-9]{1,8}$ ]]
        jq -n --arg phase "$phase" --argjson exit "$rc" --argjson seconds "$elapsed" \
          '{phase:$phase,exit:$exit,elapsed_seconds:$seconds}' >> "$WORK/phases.jsonl"
    done
    source_tree=$(git -C "$ROOT" rev-parse 'HEAD^{tree}')
    control_tree=$(git -C "$CONTROL" rev-parse 'HEAD^{tree}')
    [[ "$source_tree" =~ ^[0-9a-f]{40}$ && "$control_tree" =~ ^[0-9a-f]{40}$ ]]
    [[ "$CANDIDATE_SHA" =~ ^[0-9a-f]{40}$ && "$CONTROL_SHA" =~ ^[0-9a-f]{40}$ ]]
    jq -s '.' "$WORK"/result-*.json > "$WORK/results.json"
    jq -s '.' "$WORK/phases.jsonl" > "$WORK/phases.json"
    jq -n --arg source_sha "$CANDIDATE_SHA" --arg source_tree "$source_tree" \
      --arg control_sha "$CONTROL_SHA" --arg control_tree "$control_tree" \
      --slurpfile results "$WORK/results.json" --slurpfile phases "$WORK/phases.json" \
      '{schema:1,identity:{source_sha:$source_sha,source_tree:$source_tree,control_sha:$control_sha,control_tree:$control_tree},
        phases:$phases[0],results:$results[0],raw_logs:"runner_local_discarded",test_ceiling_seconds:60}' > "$PUBLIC/sanitized.json"
    (cd "$PUBLIC" && sha256sum sanitized.json > SHA256SUMS)
}

verdict() {
    # Expected baseline failure remains red. Never print raw diagnostics here.
    jq -e 'has("results") and (.results | length > 0) and all(.results[]; .outcome == "passed")' "$PUBLIC/sanitized.json" >/dev/null
}

phase=${1:-invalid}
case "$phase" in prepare|dependencies|configure|build|execute|collect|verdict) ;; *) printf 'phase=invalid exit=2\n' >&3; exit 2 ;; esac
key=$phase
if [[ "$phase" == execute ]]; then
    case "${SELECTOR:-}" in language-unit-analysis-zero-id|language-unit-analysis-deep|language-unit-analysis-chibi) ;; *) printf 'phase=execute exit=2\n' >&3; exit 2 ;; esac
    key="execute-$SELECTOR"
fi
started=$SECONDS
set +e
(set -euo pipefail; "$phase") 3>&- >"$WORK/$key.log" 2>&1
rc=$?
set -e
elapsed=$((SECONDS-started))
printf '%s\n' "$rc" > "$WORK/$key.exit"
printf '%s\n' "$elapsed" > "$WORK/$key.seconds"
# These fields come only from the fixed phase enum and shell numeric state.
printf 'phase=%s exit=%d elapsed_seconds=%d\n' "$key" "$rc" "$elapsed" >&3
exit "$rc"
