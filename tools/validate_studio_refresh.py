#!/usr/bin/env python3
"""Temporary Studio-only exact-head validation; independently reviewed child PR only."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
# Deliberately fail closed until coordinator supplies the reviewed published pair.
FINAL = '9d329fd4899c361f2f68774963138e5e1b797779'
FINAL_TREE = '776177eba14519ce20e79ae220906a3a044050ac'
ENGINE = '7bc6aa3cd69b29cfb006aab94d50d695007d9abe'
# These completed engine checks are reused, never reconstructed here.
PRIOR_ENGINE_EVIDENCE = {
    'run_id': 39102355375,
    'validation_revision': 'aba0cc5e0fd6745f6e1dbd100b50c0073051df2b',
    'engine_revision': ENGINE,
    'native': {'passed': 8, 'assertions': 79},
    'asan_fatal_ubsan': {'passed': 8, 'assertions': 79},
    'prior_writer_negative': 'verified intended failure',
    'serial_wasm': {'passed': 8, 'assertions': 79},
}
PRIOR_OWNERSHIP_EVIDENCE = {
    'run_id': 39106120887,
    'validation_revision': 'bdad5e64486662f502a7e343b36ceb6293a10f7e',
    'studio_revision': '4302ca9c0b3ad13a1eab82b3511d90977ef51065',
    'status': 'verified intended source_owned_gesture failure',
    'assertion_line': 221,
}
# The edge negative passed on this identical product/engine/control pair.
PRIOR_EDGE_EVIDENCE = {'run_id': 39154718997,
 'validation_revision': '01f1e11eb2ca91e2cfdb0adf2e84765737efa01e',
 'studio_revision': '9d329fd4899c361f2f68774963138e5e1b797779',
 'studio_tree': '776177eba14519ce20e79ae220906a3a044050ac',
 'engine_revision': '7bc6aa3cd69b29cfb006aab94d50d695007d9abe',
 'artifact_sha256': '48bde257c2215e1a6152f7b27543d1145aa5f3dde841eff21f704a7a34d34fc7',
 'inventory_file_sha256': '9ce3052411fa44703c752eb5b1070ee669d228a7389e978483a1ca49f669f1cb',
 'control_sha256': '788d104ec4478c3802fc0c223200ad1be14797d4e9e59b0c20f8ff6ce839526d',
 'policy_sha256': 'b9b006b3b773138d144bb028e06898565902a6177cf08e16e7c60679fb0487ef',
 'inventory': {'assertions': 264,
               'binary_sha256': '1a1fc2d8b3e1d6ffe2a23ccdcdbc3e23bef35e11abf4c49729fad0274f251a34',
               'cases': ['prototype_source_ownership'],
               'expected_diagnostics': ['Prototype source edge controls: deleted_source_retained=0 '
                                        'queued_commands_quiet=0'],
               'failed': 1,
               'frames_limit': 2,
               'negative_assertion': 'prototype_source\\.h \\(703\\): deleted_source_retained && '
                                     'queued_commands_quiet\\)',
               'passed': 0,
               'selector': '--prototype-source',
               'status': 'verified-negative',
               'suites': 0,
               'total': 1}}
# All retained files are from this dedicated allowlist directory. Logs and
# rich preflight records remain ephemeral and are never artifact inputs.
PUBLIC_DIRECTORY = None
CURRENT_STAGE = 'startup'
FAILURE_REASONS = {
    'Prior edge evidence no longer matches reviewed workload': 'reused_edge_evidence_mismatch',
    'First negative failure is not its reviewed assertion': 'unexpected_first_assertion',
    'Compiled selector case identity differs from reviewed workload': 'case_inventory_mismatch',
    'Missing unique pico summary': 'missing_or_duplicate_case_summary',
    'Wrong pico inventory': 'case_count_mismatch',
    'Candidate assertion workload failed': 'candidate_assertion_failure',
    'Infrastructure/sanitizer failure is not assertion evidence': 'infrastructure_or_sanitizer_failure',
    'Studio edge diagnostics differ from the unique reviewed result': 'edge_diagnostic_mismatch',
    'Studio authority diagnostics differ from the unique reviewed result': 'authority_diagnostic_mismatch',
    'Prototype completion marker differs': 'completion_marker_mismatch',
    'Control selector mismatch': 'control_selector_mismatch',
    'Mutation bytes differ from review': 'mutation_hash_mismatch',
    'Mutation requires exactly one reviewed original span': 'mutation_span_mismatch',
    'Counterfactual writer differs from published Git blob': 'mutation_git_blob_mismatch',
    'Reviewed Studio tree differs': 'studio_tree_mismatch',
    'Validation child changes files outside its two infrastructure files': 'validation_scope_mismatch',
    'SDK source is not the exact clean engine pin': 'sdk_revision_mismatch',
    'Missing link-time debug stripping': 'studio_build_configuration_mismatch',
    'Private engine source compiled': 'installed_sdk_boundary_violation',
    'Studio assertions disabled': 'studio_assertions_disabled',
    'Prototype binary changed': 'binary_identity_mismatch',
    'Prototype build inputs changed': 'build_identity_mismatch',
    'Prototype fixture cleanup incomplete': 'fixture_cleanup_failed',
    'Production admission/supervisor changed': 'policy_identity_mismatch',
    'Installed SDK changed during validation': 'sdk_identity_mismatch',
}

PRODUCT = 'src/editor/studio/studio_shell.c'
SUCCESS = 'Studio source edit accepted: frames=4 edits=1 undo=1 redo=1'
PROTOTYPE_SUCCESS = 'Studio prototype source ownership accepted: frames=2 prototypes=4 actors=3 checkpoints=1'
EDGE_SUCCESS = 'Prototype source edge controls: deleted_source_retained=1 queued_commands_quiet=1'
REVIEWED_FILES = {'.github/engine-ref': '0aa450a62d7457af7dab66b47d1555806707bbd621c6d7a2c909dfb4fe967875',
 'EXTRACTION.json': '9ee786222b904d633a59904001e1eb808f014d63307757fd1200d4354531a194',
 'SDK_HEADERS.json': '56e445f1f418a6f5e838ecb05645748c40167e3ec5a146871e78ddedb02d874b',
 'cmake/studio_sources.cmake': '02705e472a50898a7068f5f7993c7479870ab1e247eaa3a1cd116be5faa35aca',
 'src/editor/collab/actions_scene.c': 'ccd7e001355c6b1e00641b3190e995f951ded077c77d6569743d91f312ca6d6e',
 'src/editor/collab/collab.c': 'af5959e30bc7ec83da31547d9d1d2ec1169df84bf70878d60209e3df8ec2a353',
 'src/editor/collab/collab.h': '5ad2e237b948e3818ea595e9f38efb0557e51dee09f190c2bd176fc4f6023d4b',
 'src/editor/collab/protocol.h': 'd30ffb61a1d099eb5d7399d9f3f09020c94cce0476bf758ee7ce4c1b9fd1e92d',
 'src/editor/documents/asset_code.c': '87ffc2770a876ae48da68f2f979f2470e6effc238a90df4e0e0f0f1435f4103d',
 'src/editor/documents/asset_code.h': '012f40bdfc5a1d5b5d71aa9c2f6a6ff988c154b4b83b4890316283ac76400201',
 'src/editor/documents/prototype_source_input.c': 'c4bd1cb21c3aa3199e657b9abe57fb954486be8bf9186271489321244b511b68',
 'src/editor/documents/prototype_source_input.h': '76772f93dc0458baf0695b85f7851c9a521c19bb8b4afc0d0f52edf3c4668971',
 'src/editor/documents/scene_disk.c': '61df3e4276a2d24f311727aea2056a5e314a15f6f3ea9eb0366f320e78db5e65',
 'src/editor/documents/scene_documents.c': '829844d7c5f026835c33b608f2a4611586bf8b87959674a5586ca2c0a87acee2',
 'src/editor/documents/scene_history.c': 'c85f0c20e977bb1e679a6904b0812eee01e148b2f1390469f6a509355bccc7ab',
 'src/editor/documents/scene_history.h': 'b4ff9050dd63cb59ad410efb7aa9a384eca64ff549766597535a8eba2e63946d',
 'src/editor/documents/scene_prototype_actions.c': '02184147bc3e50eecf4703520ca13435f1128b1f95f9a4c498fbf1a908cabba3',
 'src/editor/documents/scene_prototypes.c': '4374bdeffcb95e4cdab94434af50123a811781cd4b08044c814a5c01f936b492',
 'src/editor/studio/studio_code.c': 'b8a6a580373cf1aa5caeb7311effbc309d62ff6dc3dc7370bab5a10ea3c8c37d',
 'src/editor/studio/studio_internal.h': '3f99a1b03d39441bc2cd486e36c2662037cd43055242c8953d1a0f27b7c4e4da',
 'src/editor/studio/studio_projects.c': '4c02c4fab9462e86aa6ef40f2d3cd75842cbc4c69dade6d43d3d5e422b083a47',
 'src/editor/studio/studio_properties.c': 'd20368e746e6070e7fdc8942c5e73d5504f89ffafd62e107998a4bf4e45e34a4',
 'src/editor/studio/studio_properties_view.c': '728ddaa0c20b1806bf0a8e375cd2c39b8bf6a2f3f63bb5c9cede856ac40374cb',
 'src/editor/studio/studio_prototype_actions.c': '61f768872d015bb3d9ea9e140a077434b86cb5e29e88607cd408fd730fb5373f',
 'src/editor/studio/studio_shell.c': '04bf2ee8660e9d4fe10c6deac391eea6a9b5315cabe7b653a369fbc929dd5202',
 'src/editor/workspace/scene_actors.c': '2e18d5ca42dcfb77671a0937bca2d0018f865239e27db13dfdac2caa4296f9b4',
 'src/editor/workspace/scene_authoring.c': '01af66a689b2141df3bdad469360f92da3b4c9d4acc8751b21590e900c2a7a37',
 'src/editor/workspace/scene_authoring.h': '73d7890233f253eca58066ce9aa1b44b4ac2c47f9a30a4ec42ff457dd89e3c47',
 'src/editor/workspace/scene_checkpoint.c': '3a80436d6f4d135264549f018c95fbb83e1c6e5fa5abf646ccb743be58aa7581',
 'src/editor/workspace/scene_internal.h': 'ce497c867a2fe4419801d1e7b2cc54c66bafd4f8f3f4f537e41139348d30ce9f',
 'src/editor/workspace/scene_tabs.c': '8c4f04ed722496c82b54b924d7570a04ccc0d5408dc6931a08bc9a97d5e44131',
 'tests/acceptance/README.md': 'e258405389d955a43240c69a03b7dd3499c68a4e90bbfbff0babc7355742e086',
 'tests/acceptance/main.c': 'f62699332037f6a677ae713bc5f6fc0cf586c744d8b2860c89564a2bf0309c00',
 'tests/acceptance/prototype_source.h': '61c08b44b025cd63494b5c9097e2d35f928c2e8d3dc16e1f32ec4b45e9f9688f',
 'tests/acceptance/prototype_source_authority.h': '4edb6d7e4d0b6e025d660c5ce9ad43de03e1b329fd7643b7eb5e56e777c3490c',
 'tests/acceptance/prototype_source_capture.h': '525a1436a4673e4c7c51fbe5eec2cd24d48276aa3c7c30fe15e45bca5dded2f9',
 'tests/acceptance/scene_move.h': '9a84ba7af42c74f384f0bd4db432b4d29aad66a580317c22a3f35bcdb29e07f2',
 'tests/acceptance/source_action_outcomes.h': 'efdb4571214e15a104428f68e0fcf8b9835e222264908fca076a79898a098f32',
 'tests/acceptance/source_discard.h': '38eb2fb91b2385b8e98e0694e78fda83ee68e7ab62979712169663c5af55187b'}
OWNERSHIP_PREFIX_SHA256 = '52bfca49f933549aae955962c692f9bca02bd6e27222aed69945f857c070d88c'
EDGE_CONTROL = {'assertion': 'prototype_source\\.h \\(703\\): deleted_source_retained && queued_commands_quiet\\)',
 'files': {'src/editor/documents/asset_code.c': {'git_blob': '3079f5a8a07e82a8e73e8cc3890e13f4082e68f6',
                                                 'replacements': [{'after': '',
                                                                   'before': '    if (c->file_missing) {\n'
                                                                             '        return true;\n'
                                                                             '    }\n'}],
                                                 'sha256': '181e210c3ea64c97074e0d96bac9a17cc84258639c84889eb3f88efcfb773702'},
           'src/editor/studio/studio_properties.c': {'git_blob': 'a09c785fd5f7adbe1409cf743875760b28f23614',
                                                     'replacements': [{'after': '        '
                                                                                'scene_prototype_asset '
                                                                                '*asset = '
                                                                                'find_prototype(scene, '
                                                                                'target);\n'
                                                                                '        if (added > 0 && '
                                                                                'asset && '
                                                                                'asset->source_not_applied) '
                                                                                '{',
                                                                       'before': '        if (added == '
                                                                                 'APP_PROTOTYPE_EDIT_NOT_APPLIED) '
                                                                                 '{'},
                                                                      {'after': '    if (result < 0 || '
                                                                                '(result > 0 && '
                                                                                '!asset->source_not_applied)) '
                                                                                '{',
                                                                       'before': '    if (result == '
                                                                                 'APP_PROTOTYPE_EDIT_PENDING) '
                                                                                 '{\n'
                                                                                 '        return;\n'
                                                                                 '    }\n'
                                                                                 '    if (result < 0) {'},
                                                                      {'after': '    if (result > 0) {',
                                                                       'before': '    if (result == '
                                                                                 'APP_PROTOTYPE_EDIT_NOT_APPLIED) '
                                                                                 '{'}],
                                                     'sha256': '88812cce2791a700932a476362c84ebbf660c9d2d23246c25f2c1416322d1e20'}},
 'required_markers': ['Prototype source edge controls: deleted_source_retained=0 queued_commands_quiet=0']}
AUTHORITY_CONTROL = {'assertion': 'prototype_source_authority\\.h \\(837\\): test\\.code_authority && '
              'test\\.property_authority\\)',
 'files': {'src/editor/documents/prototype_source_input.c': {'after': '    if (gate == '
                                                                      'COLLAB_ACTION_PENDING) {\n'
                                                                      '        yyjson_doc *optimistic = '
                                                                      'yyjson_read(json, strlen(json), 0);\n'
                                                                      '        char optimistic_error[256] = '
                                                                      '"";\n'
                                                                      '        '
                                                                      '(void)scene_source_input_apply(scene, '
                                                                      'yyjson_doc_get_root(optimistic), '
                                                                      'optimistic_error, sizeof '
                                                                      'optimistic_error);\n'
                                                                      '        yyjson_doc_free(optimistic);\n'
                                                                      '        input->waiting = '
                                                                      'collab_pending_action(&input->sent);',
                                                             'before': '    if (gate == '
                                                                       'COLLAB_ACTION_PENDING) {\n'
                                                                       '        input->waiting = '
                                                                       'collab_pending_action(&input->sent);',
                                                             'git_blob': '2cc916784135e35f70f31b5437ed1a1f1e1b3dd1',
                                                             'sha256': '067cd0df960296e74823f05f8f742438e2e3d7e93241666ea02e73e833dcb197'}},
 'required_markers': ['Prototype source authority: Code unchanged=0, Properties unchanged=0',
                      'Prototype source intent: retry_held=-1',
                      'Prototype source frame: held=-1, live=-1',
                      'Prototype source disk: conflict=-1']}
INTENT_CONTROL = {'assertion': 'prototype_source_authority\\.h \\(839\\): test\\.retry_held == 1\\)',
 'files': {'src/editor/documents/prototype_source_input.c': {'after': '    bool live = '
                                                                      '!asset_code_runtime_held(asset->source);\n',
                                                             'before': '    bool live = input->live;\n',
                                                             'git_blob': '6f2a3250834b25f0d4856b9d5f18d4310a39a008',
                                                             'sha256': 'f866cb067c2df543b25bbfc67020bf96e7366fcbb039d33772320bcd86bbb150'}},
 'required_markers': ['Prototype source authority: Code unchanged=1, Properties unchanged=1',
                      'Prototype source intent: retry_held=0',
                      'Prototype source frame: held=-1, live=-1',
                      'Prototype source disk: conflict=-1']}
FRAME_CONTROL = {'assertion': 'prototype_source_authority\\.h \\(840\\): test\\.frame_held == 1 && test\\.frame_live == 1\\)',
 'files': {'src/editor/studio/studio_code.c': {'after': '                    if (!s->prefs.code_apply_live '
                                                        '&& asset_code_canonical_dirty(code)) {\n'
                                                        '                        '
                                                        'asset_code_hold_runtime(code, true);\n'
                                                        '                    }\n'
                                                        '                    '
                                                        'scene_prototype_source_sync(s->scene, prototype, '
                                                        's->prefs.code_apply_live);\n',
                                               'before': '                    '
                                                         'scene_prototype_source_sync(s->scene, prototype, '
                                                         '!asset_code_runtime_held(code));\n',
                                               'git_blob': '42055b451bfd84dcd4e8c39d35286ec6a280aa92',
                                               'sha256': 'c4753f808a43513155b507b1e8fef0992761c63f0362f5f15a8831326c733b65'}},
 'required_markers': ['Prototype source authority: Code unchanged=1, Properties unchanged=1',
                      'Prototype source intent: retry_held=1',
                      'Prototype source frame: held=0, live=0',
                      'Prototype source disk: conflict=1']}
DISK_CONTROL = {'assertion': 'prototype_source_authority\\.h \\(841\\): test\\.disk_conflict == 1\\)',
 'files': {'src/editor/documents/prototype_source_input.c': {'after': '        if '
                                                                      '(asset_code_prepare_refresh(asset->source, '
                                                                      '&disk, error, sizeof error) &&\n'
                                                                      '            disk.text) {\n',
                                                             'before': '        if '
                                                                       '(!asset_code_prepare_refresh(asset->source, '
                                                                       '&disk, error, sizeof error)) {\n'
                                                                       '            '
                                                                       'asset_code_reject_static(asset->source, '
                                                                       'error);\n'
                                                                       '        } else if (disk.text) {\n',
                                                             'git_blob': '3cf332acd750dfa03212646565946aeb955f2553',
                                                             'sha256': 'a387a3ba07cd9304a849867c918154c61c2bbf5ef2a9cc8e3d66e517b96d940a'}},
 'required_markers': ['Prototype source authority: Code unchanged=1, Properties unchanged=1',
                      'Prototype source intent: retry_held=1',
                      'Prototype source frame: held=1, live=1',
                      'Prototype source disk: conflict=0']}
AUTHORITY_SUCCESS = 'Studio prototype source authority accepted: frames=2 prototypes=2 actors=0 checkpoints=1 incoming=70 outgoing=98'
AUTHORITY_DIAGNOSTIC = 'Prototype source authority: Code unchanged=1, Properties unchanged=1'
INTENT_SUCCESS = 'Prototype source intent: retry_held=1'
FRAME_SUCCESS = 'Prototype source frame: held=1, live=1'
DISK_SUCCESS = 'Prototype source disk: conflict=1'
CAPTURE_SUCCESS = 'Prototype source controls: notice_retained=1 focus_retained=1 enter_edits_draft=1'
CONTROLS = {
    'edges-red': EDGE_CONTROL,
    'authority-red': dict(AUTHORITY_CONTROL, authority=True),
    'intent-red': dict(INTENT_CONTROL, authority=True),
    'frame-red': dict(FRAME_CONTROL, authority=True),
    'disk-red': dict(DISK_CONTROL, authority=True),
}
PROHIBITED = ('Studio UI:', 'runtime frame failed', 'Could not initialize')
INFRA = {'.github/workflows/ci.yml', 'tools/validate_studio_refresh.py'}
POLICY = ('tools/agent.py', 'tools/test_policy.py', 'test/execution_selectors.h')
# The sole tracked link in FINAL's reviewed Git tree; never dereference it in evidence.
SOURCE_LINKS = {'CLAUDE.md': {'type': 'symlink', 'git_mode': '120000',
                             'target': 'AGENTS.md',
                             'git_blob': '47dc3e3d863cfb5727b87d785d09abf9743c0a72'}}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def canonical(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def public_status(name, status, **counts):
    require(re.fullmatch(r'[a-z0-9-]+', name), 'Invalid public status name')
    require(status in ('completed', 'failed', 'executed', 'verified',
                       'verified-negative', 'blocked-or-failed'), 'Invalid public status')
    require(all(isinstance(value, (int, bool)) for value in counts.values()),
            'Public status must contain only verified numeric counts')
    if PUBLIC_DIRECTORY is not None:
        save(PUBLIC_DIRECTORY / (name + '.json'), {'status': status, **counts})
    print(name + ': ' + status, flush=True)


def stage(name):
    global CURRENT_STAGE
    require(re.fullmatch(r'[a-z0-9-]+', name), 'Invalid validation stage')
    CURRENT_STAGE = name


def failure_status(error):
    if isinstance(error, RuntimeError):
        reason = FAILURE_REASONS.get(str(error), 'validation_requirement_failed')
    elif isinstance(error, subprocess.CalledProcessError):
        reason = 'subprocess_failed'
    elif isinstance(error, OSError):
        reason = 'filesystem_or_process_error'
    elif isinstance(error, (ValueError, KeyError, TypeError)):
        reason = 'invalid_structured_input'
    else:
        reason = 'unexpected_helper_error'
    if PUBLIC_DIRECTORY is not None:
        save(PUBLIC_DIRECTORY / 'failure.json', {
            'status': 'blocked-or-failed', 'stage': CURRENT_STAGE, 'reason': reason,
        })
    public_status('validation', 'blocked-or-failed')


def studio_failure_locations(source, output):
    # Only names present in the reviewed public Studio fixture are eligible.
    # Never retain assertion expressions, arbitrary paths or engine diagnostics.
    headers, cases = set(), set()
    for path in (source / 'tests/acceptance').rglob('*'):
        if path.is_file() and not path.is_symlink() and path.suffix in ('.c', '.h'):
            headers.add(path.relative_to(source).as_posix())
            cases.update(re.findall(r'\bTEST_CASE\(([A-Za-z0-9_]+)\)', path.read_text()))
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    # pico prints case banners to buffered stdout and failures to stderr.
    # Merged non-TTY output can put failures before their banner. Only a
    # single reviewed case is attributable without guessing stream ordering.
    running = re.findall(r'Running: ([A-Za-z0-9_]+)', plain)
    case = running[0] if len(running) == 1 and running[0] in cases else None
    failures, failure_markers = [], 0
    for line in plain.splitlines():
        failure_markers += '(FAILED:' in line
        failed = re.search(r'\(FAILED: ([^\r\n]+?) \((\d+)\):', line)
        if not failed:
            continue
        file = failed[1].replace('\\', '/')
        eligible = [name for name in headers if file == name or file.endswith('/' + name) or
                    ('/' not in file and file == Path(name).name)]
        if len(eligible) == 1 and 0 < int(failed[2]) <= 1000000:
            failures.append({'case': case, 'file': eligible[0], 'line': int(failed[2])})
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    return {'failures': failures[:16], 'additional_failures': max(0, len(failures) - 16),
            'failure_markers': failure_markers, 'unmapped_failures': failure_markers - len(failures),
            'summary_count': len(summaries),
            'summary': dict(zip(('passed', 'failed', 'total', 'suites', 'assertions'), map(int, summaries[0])))
                       if len(summaries) == 1 else None}


def git(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments], text=True, stderr=subprocess.PIPE).strip()


def clean_checkout(root, revision):
    require(git(root, 'rev-parse', 'HEAD') == revision, f'Wrong checkout: {root}')
    require(not git(root, 'status', '--porcelain', '--untracked-files=normal'), f'Dirty checkout: {root}')


def snapshot(root, names, links=None):
    names = set(names)
    if links is not None:
        require(set(links) <= names, 'Missing reviewed source link')
        for directory, directories, files in os.walk(root, followlinks=False):
            if Path(directory) == root:
                directories[:] = [name for name in directories if name != '.git']
            for name in directories + files:
                path = Path(directory) / name
                if path.is_symlink():
                    relative = path.relative_to(root).as_posix()
                    require(relative in names and relative in links,
                            'Unreviewed or untracked source link: ' + relative)
    result = {}
    for name in sorted(names):
        path = root / name
        if links is not None and name in links:
            expected = links[name]
            require(path.is_symlink(), 'Source link type changed: ' + name)
            target = os.readlink(path)
            require(target == expected['target'], 'Source link target changed: ' + name)
            destination = path.parent / target
            require(not Path(target).is_absolute() and '..' not in Path(target).parts and
                    destination.relative_to(root).as_posix() in names and
                    destination.is_file() and not destination.is_symlink() and
                    root.resolve() in destination.resolve().parents,
                    'Source link target is not a tracked in-tree regular file: ' + name)
            content = os.fsencode(target)
            blob = hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()
            require(blob == expected['git_blob'], 'Source link Git blob changed: ' + name)
            result[name] = dict(expected)
            continue
        require(not path.is_symlink() and path.is_file(), f'Non-regular source: {name}')
        result[name] = digest(path)
    return result


def tracked(root):
    return set(filter(None, git(root, 'ls-files', '-z').split('\0')))


def assertions_enabled(arguments):
    defined = False
    tokens = iter(arguments)
    for token in tokens:
        if token in ('-D', '-U'):
            token += next(tokens, '')
        if token == '-DNDEBUG' or token.startswith('-DNDEBUG='):
            defined = True
        elif token == '-UNDEBUG':
            defined = False
    return not defined


def extraction(source):
    value = json.loads((source / 'EXTRACTION.json').read_text())
    for name, row in value['files'].items():
        # sha256 is historical extraction provenance; current_source_sha256 hashes
        # raw engine bytes before consumer replacements. Only consumer_sha256
        # asserts current Studio bytes; the complete source has its own Git snapshot.
        if 'consumer_sha256' not in row:
            continue
        checksum = row['consumer_sha256']
        require(isinstance(checksum, str) and re.fullmatch(r'[0-9a-f]{64}', checksum),
                'Malformed consumer checksum: ' + name)
        require(digest(source / name) == checksum,
                f'Extraction checksum differs: {name}')
    return value


def reviewed_pins():
    for name, value in (('Studio', FINAL), ('Studio tree', FINAL_TREE), ('engine', ENGINE)):
        require(re.fullmatch(r'[0-9a-f]{40}', value), 'Final reviewed ' + name + ' SHA has not been supplied')
    require(REVIEWED_FILES, 'Reviewed source identities have not been supplied')
    for variant in CONTROLS.values():
        require(variant['files'] and variant['assertion'] != 'PENDING', 'Studio control is not frozen')
        require(all(marker != 'PENDING' for marker in variant.get('required_markers', [])),
                'Studio control diagnostics are not frozen')
    for control in (mutation for variant in CONTROLS.values()
                    for mutation in variant['files'].values()):
        require(re.fullmatch(r'[0-9a-f]{64}', control['sha256']), 'Reviewed mutation has not been supplied')
        for change in control.get('replacements', [control]):
            require(change['before'] != change['after'] and change['before'] != 'PENDING', 'Mutation is not frozen')


def mutate(content, control):
    result = content
    for change in control.get('replacements', [control]):
        require(result.count(change['before']) == 1, 'Mutation requires exactly one reviewed original span')
        result = result.replace(change['before'], change['after'])
    data = result.encode()
    require(hashlib.sha256(data).hexdigest() == control['sha256'], 'Mutation bytes differ from review')
    if 'git_blob' in control:
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        require(blob == control['git_blob'], 'Counterfactual writer differs from published Git blob')
    return result


def source_inputs(args):
    stage('source-preflight')
    reviewed_pins()
    clean_checkout(args.source, FINAL)
    require(git(args.source, 'rev-parse', 'HEAD^{tree}') == FINAL_TREE, 'Reviewed Studio tree differs')
    clean_checkout(args.engine, ENGINE)
    clean_checkout(args.validation, git(args.validation, 'rev-parse', 'HEAD'))
    require((args.source / '.github/engine-ref').read_text().strip() == ENGINE, 'Engine pin changed')
    for name, checksum in REVIEWED_FILES.items():
        require(digest(args.source / name) == checksum, 'Unreviewed source: ' + name)
    prefix = ''.join((args.source / 'tests/acceptance/prototype_source.h').read_text().splitlines(True)[:221])
    require(hashlib.sha256(prefix.encode()).hexdigest() == OWNERSHIP_PREFIX_SHA256,
            'Prior ownership negative no longer covers the unchanged exercised fixture prefix')
    source_names = tracked(args.source)
    final_files = snapshot(args.source, source_names, SOURCE_LINKS)
    validation_files = snapshot(args.validation, tracked(args.validation), SOURCE_LINKS)
    require({k: v for k, v in final_files.items() if k not in INFRA} ==
            {k: v for k, v in validation_files.items() if k not in INFRA},
            'Validation child changes files outside its two infrastructure files')
    require(digest(Path(__file__).resolve()) == validation_files['tools/validate_studio_refresh.py'],
            'Running helper differs from validation checkout')
    final_manifest = extraction(args.source)
    for name in REVIEWED_FILES.keys() & final_manifest['files'].keys():
        require(final_manifest['files'][name].get('consumer_sha256') == digest(args.source / name),
                'Edited extracted file lacks current consumer checksum: ' + name)
    sources = {}
    records = {}
    for label, control in CONTROLS.items():
        red = args.output / (label + '-source')
        red.mkdir()
        for name in source_names:
            target = red / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source / name, target, follow_symlinks=False)
        manifest = json.loads(json.dumps(final_manifest))
        for product, mutation in control['files'].items():
            require(product in source_names and product in REVIEWED_FILES,
                    'Studio control lacks tracked reviewed source identity: ' + product)
            (red / product).write_text(mutate((red / product).read_text(), mutation))
            if product in manifest['files']:
                manifest['files'][product]['consumer_sha256'] = mutation['sha256']
        extraction_changed = manifest != final_manifest
        if extraction_changed:
            save(red / 'EXTRACTION.json', manifest)
        require(extraction(red) == manifest, 'Studio counterfactual extraction audit failed')
        red_files = snapshot(red, source_names, SOURCE_LINKS)
        changed = {name for name in source_names if red_files[name] != final_files[name]}
        require(changed == set(control['files']) | ({'EXTRACTION.json'} if extraction_changed else set()),
                'Studio red has an unintended source difference')
        records[label] = {'files': red_files, 'counterfactual': True, 'dirty': True,
                          'changed_files': sorted(changed), 'control': control}
        sources[label] = red
    save(PUBLIC_DIRECTORY / 'identity.json', {
        'studio_revision': FINAL, 'studio_tree': git(args.source, 'rev-parse', 'HEAD^{tree}'),
        'validation_revision': git(args.validation, 'rev-parse', 'HEAD'),
        'helper_sha256': digest(Path(__file__).resolve()),
        'workflow_sha256': digest(args.validation / '.github/workflows/ci.yml'),
        'engine_revision': ENGINE, 'engine_tree': git(args.engine, 'rev-parse', 'HEAD^{tree}'),
        'reused_engine_run': PRIOR_ENGINE_EVIDENCE['run_id'],
        'reused_ownership_run': PRIOR_OWNERSHIP_EVIDENCE['run_id'],
        'studio_reviewed_files': REVIEWED_FILES,
    })
    save(args.output / 'evidence/source-identity.json', {
        'final_revision': FINAL, 'final_tree': git(args.source, 'rev-parse', 'HEAD^{tree}'),
        'final_files': final_files,
        'studio_controls': records,
    })
    return sources, final_files


def sdk_identity(args):
    stage('sdk-preflight')
    # Existing production package verifier checks identity, all files and symlinks.
    from package_sdk import previous_package
    with (args.output / 'logs/sdk-verification.log').open('w') as log:
        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            manifest = previous_package(args.sdk, 'sdk')
    require(manifest['source']['revision'] == ENGINE and manifest['source']['dirty'] is False,
            'SDK source is not the exact clean engine pin')
    require(manifest['requirements']['system'] == 'Linux', 'SDK is not native Linux')
    require(manifest['build']['configuration'] == 'Debug', 'Acceptance SDK must preserve Debug assertions')
    require(manifest['build']['native_backend'] is True, 'SDK lacks actual SLJIT backend')
    engine_objects = [row for row in manifest['build']['inputs']['objects'].values()
                      if any(arg.startswith('@source/src/') for arg in row['arguments'])]
    require(engine_objects and all(assertions_enabled(row['arguments']) for row in engine_objects),
            'SDK engine object assertions are disabled or unproved')
    require(all(not any(arg.startswith('-fsanitize=') for arg in row['arguments']) for row in engine_objects),
            'Unexpected sanitized SDK for bounded acceptance')
    result = {'build_id': manifest['build_id'], 'manifest_sha256': digest(args.sdk / 'share/chibi/package.json'),
              'verified_files': len(manifest['files']), 'verified_engine_objects': len(engine_objects),
              'source': manifest['source'], 'configuration': manifest['build']['configuration'],
              'sdk_config_sha256': digest(args.sdk / 'lib/cmake/Chibi/ChibiConfig.cmake')}
    save(PUBLIC_DIRECTORY / 'sdk-identity.json', {
        'build_id': result['build_id'], 'package_sha256': result['manifest_sha256'],
        'exact_engine_pin_verified': True, 'debug_assertions_verified': True,
        'native_backend_verified': True, 'sanitizers': False,
    })
    save(args.output / 'evidence/sdk-identity.json', result)
    return result


def build_command(args, name, command):
    stage(name)
    path = args.output / 'logs' / (name + '.log')
    with path.open('w') as log:
        code = subprocess.run(command, cwd=args.engine, stdout=log, stderr=subprocess.STDOUT).returncode
    public_status(name, 'completed' if code == 0 else 'failed', exit_code=code)
    require(code == 0, f'{name} build failed')
    return code


def within(path, root):
    return path == root or root in path.parents


def evidence_bounds(evidence, compile_bytes):
    require(0 < evidence['binary_bytes'] <= 64 * 1024 * 1024, 'Acceptance binary bound')
    require(0 < compile_bytes <= 64 * 1024 * 1024, 'Compilation evidence bound')
    require(8 <= len(evidence['files']) <= 32 and
            all(0 <= row['bytes'] <= 256 * 1024 for row in evidence['files'].values()),
            'Acceptance source inventory bound')
    resources = evidence['resources']
    require(set(resources) == {'studio', 'ui'}, 'Unexpected resource inventory')
    for row in resources.values():
        require(isinstance(row, list) and len(row) == 3 and
                type(row[0]) is int and 0 < row[0] <= 320 and
                type(row[1]) is int and 0 < row[1] <= 1024 * 1024 and
                isinstance(row[2], str) and re.fullmatch(r'[0-9a-f]{64}', row[2]),
                'Resource preflight bound')
    require(len((json.dumps(evidence, indent=2, sort_keys=True) + '\n').encode()) <= 1024 * 1024,
            'Acceptance evidence bound')


def build_variant(args, name, source, sdk):
    build = args.output / (name + '-build')
    configured = build_command(args, name + '-configure', [
        'cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Debug', '-DCHIBI_STUDIO_BUILD_ACCEPTANCE=ON',
        '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--strip-debug',
        '-DChibi_DIR=' + str(args.sdk / 'lib/cmake/Chibi')])
    built = build_command(args, name + '-build', [
        'cmake', '--build', str(build), '--target', 'studio-acceptance', '--parallel', '2'])
    compile_file = build / 'compile_commands.json'
    require(compile_file.is_file() and not compile_file.is_symlink() and
            0 < compile_file.stat().st_size <= 64 * 1024 * 1024, 'Compilation evidence bound')
    commands = json.loads(compile_file.read_text())
    require(commands, 'Missing compilation evidence')
    own_entries = 0
    for entry in commands:
        arguments = entry.get('arguments') or shlex.split(entry['command'])
        file = (Path(entry['directory']) / entry['file']).resolve()
        if within(file, source):
            own_entries += 1
            require(assertions_enabled(arguments), 'Studio assertions disabled')
        require(not within(file, args.engine) or within(file, args.sdk), 'Private engine source compiled')
        tokens = iter(arguments)
        for token in tokens:
            include = None
            include_flags = ('-isystem', '-iquote', '-idirafter', '-include', '-imacros', '-I')
            require(not token.startswith('@'), 'Unexpanded compiler response file')
            if token in include_flags:
                include = next(tokens, '')
            else:
                for flag in include_flags:
                    if token.startswith(flag) and len(token) > len(flag):
                        include = token[len(flag):]
                        break
            if include is not None:
                path = (Path(entry['directory']) / include).resolve()
                require(not within(path, args.engine) or within(path, args.sdk),
                        f'Private engine source include: {path}')
    require(own_entries, 'No Studio compilation entries')
    link = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
                                    'bin/studio-acceptance'], text=True, stderr=subprocess.PIPE)
    require('-Wl,--strip-debug' in link.splitlines()[-1], 'Missing link-time debug stripping')
    binary = build / 'bin/studio-acceptance'
    require(binary.is_file() and not binary.is_symlink() and 0 < binary.stat().st_size <= 64 * 1024 * 1024,
            'Acceptance binary exceeds unchanged 64 MiB preflight bound')
    paths = list((source / 'tests/acceptance').rglob('*'))
    require(not any(p.is_symlink() for p in paths), 'Acceptance symlink')
    files = {p.relative_to(source).as_posix(): {'bytes': p.stat().st_size, 'sha256': digest(p)}
             for p in paths if p.is_file()}
    # Restore before freezing the resource identity; the fixture migrates this copy.
    shutil.copy2(source / 'projects/studio/project.kdl', build / 'share/chibi/studio/project.kdl')
    resources = {}
    for resource in ('studio', 'ui'):
        root = build / 'share/chibi' / resource
        paths = list(root.rglob('*'))
        require(not any(p.is_symlink() for p in paths), 'Resource symlink')
        require(all(0 < p.stat().st_size <= 1024 * 1024 for p in paths if p.is_file()),
                'Resource file bound')
        members = {p.relative_to(root).as_posix(): digest(p) for p in paths if p.is_file()}
        resources[resource] = [len(members), sum((root / p).stat().st_size for p in members), canonical(members)]
    native = {'source': str(source), 'sdk': str(args.sdk), 'configure_exit': configured,
              'build_exit': built, 'private_engine_source_include': False,
              'sdk_config_sha256': sdk['sdk_config_sha256']}
    evidence = {'source': str(source), 'build': str(build), 'sdk': str(args.sdk),
                'binary': str(binary), 'binary_sha256': digest(binary),
                'binary_bytes': binary.stat().st_size, 'extraction_sha256': digest(source / 'EXTRACTION.json'),
                'compile_commands_sha256': digest(build / 'compile_commands.json'),
                'frames': 4, 'hidden': True, 'window': [1920, 1080], 'success': SUCCESS,
                'files': files, 'resources': resources, 'candidate_revision': FINAL,
                'counterfactual': name in CONTROLS, 'source_dirty': name in CONTROLS, 'variant': name,
                'product_sha256': digest(source / PRODUCT), 'sdk_build_id': sdk['build_id'],
                'assertions_enabled': True, 'private_engine_source_include': False}
    evidence_bounds(evidence, compile_file.stat().st_size)
    evidence_path = args.output / 'evidence' / (name + '-acceptance.json')
    save(evidence_path, evidence)
    save(args.output / 'evidence' / (name + '-native.json'), native)
    save(PUBLIC_DIRECTORY / (name + '-build-identity.json'), {
        'binary_sha256': evidence['binary_sha256'], 'binary_bytes': evidence['binary_bytes'],
        'fixture_files': evidence['files'], 'variant': name,
        'assertions_enabled': True, 'private_engine_source_include': False,
    })
    return build, evidence_path, native, evidence


def fixture_directories(source):
    prefixes = set()
    for path in (source / 'tests/acceptance').rglob('*'):
        if path.is_file() and path.suffix in ('.c', '.h'):
            prefixes.update(re.findall(r'"/tmp/([^"/]*?)XXXXXX"', path.read_text()))
    return {str(path) for prefix in prefixes for path in Path('/tmp').glob(prefix + '*')}


def supervised(args, name, commands, env):
    stage(name)
    from test_policy import run_commands
    path = args.output / 'logs' / (name + '.log')
    fixtures_before = fixture_directories(args.source)
    with path.open('w') as log:
        code = run_commands(commands, seconds=60, env=env, output=log)
    output = path.read_text()
    observations = studio_failure_locations(args.source, output)
    if code or observations['failures']:
        save(PUBLIC_DIRECTORY / (name + '-observations.json'), {
            'exit_code': code, **observations,
            'infrastructure_marker': any(marker in output for marker in PROHIBITED),
        })
    new_fixtures = sorted(fixture_directories(args.source) - fixtures_before)
    save(args.output / 'evidence' / (name + '-result.json'), {
        'exit_code': code, 'log_sha256': digest(path), 'new_hardcoded_tmp_fixtures': new_fixtures})
    require(not new_fixtures, 'Test left scoped /tmp fixtures (retained, not deleted): ' + ', '.join(new_fixtures))
    public_status(name, 'executed', exit_code=code)
    return code, output


def graphics_environment(args):
    env = os.environ.copy()
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'SDL_VIDEODRIVER'):
        env.pop(key, None)
    paths = subprocess.check_output(['dpkg-query', '-L', 'mesa-vulkan-drivers'], text=True, stderr=subprocess.PIPE).splitlines()
    icds = [Path(p) for p in paths if re.fullmatch(r'lvp_icd.*\.json', Path(p).name)]
    require(len(icds) == 1 and icds[0].is_file(), 'Expected one installed Mesa Lavapipe ICD')
    for directory in ('scratch', 'cache', 'prefs'):
        (args.output / directory).mkdir()
    env.update(VK_DRIVER_FILES=str(icds[0]), SDL_GPU_DRIVER='vulkan', SDL_VIDEO_DRIVER='x11',
               SDL_AUDIODRIVER='dummy', SDL_LOGGING='gpu=info', TMPDIR=str(args.output / 'scratch'),
               XDG_CACHE_HOME=str(args.output / 'cache'), MESA_SHADER_CACHE_DIR=str(args.output / 'cache'),
               CHIBI_PREFS_DIR=str(args.output / 'prefs'))
    code, output = supervised(args, 'vulkan-discovery', [[sys.executable, 'tools/agent.py', 'doctor']], env)
    require(code == 0, 'Vulkan discovery failed; no backend fallback')
    discoveries = []
    for line in output.splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(row, dict) and row.get('scope') == 'discovery_only_not_render_validation':
            discoveries.append(row)
    require(len(discoveries) == 1, 'Missing or ambiguous Vulkan doctor result')
    health = discoveries[0]
    require(health.get('backend') == 'Vulkan' and health.get('status') == 'enumerated' and
            health.get('acceleration') == 'software' and health.get('devices') and
            all(row.get('type') == 4 for row in health['devices']),
            'Expected an enumerated software Vulkan device')
    public_status('graphics-discovery', 'verified')
    save(args.output / 'evidence/backend.json', {'icd': str(icds[0]), 'icd_sha256': digest(icds[0]),
         'mesa_package': subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'mesa-vulkan-drivers'], text=True, stderr=subprocess.PIPE),
         'environment': {k: env[k] for k in ('VK_DRIVER_FILES', 'SDL_GPU_DRIVER', 'SDL_VIDEO_DRIVER')},
         'discovery': health, 'scope': 'discovery only; acceptance must actually render'})
    return env


def gpu_test(args, name, source, artifact, base_env):
    build, path, native, evidence = artifact
    record = args.engine / '.chibi/architecture/external-studio-native-acceptance.json'
    record.parent.mkdir(parents=True, exist_ok=True)
    save(record, native)
    require(digest(source / 'projects/studio/project.kdl') ==
            digest(build / 'share/chibi/studio/project.kdl'), 'Bundled manifest changed after resource audit')
    require(digest(build / 'bin/studio-acceptance') == evidence['binary_sha256'], 'Binary changed after audit')
    env = dict(base_env, CHIBI_STUDIO_SOURCE=str(source), CHIBI_STUDIO_BUILD=str(build),
               CHIBI_STUDIO_ACCEPTANCE_EVIDENCE=str(path))
    code, output = supervised(args, name + '-source-edit', [[
        'xvfb-run', '-a', sys.executable, 'tools/agent.py', 'gpu-test', '--', '--studio-source-edit']], env)
    reports = []
    for line in output.splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(row, dict) and row.get('coverage') == 'focused-iteration':
            reports.append(row)
    require(len(reports) == 1, 'Missing or ambiguous agent execution result')
    report = reports[0]
    require(not any(marker in output for marker in PROHIBITED), 'UI/backend failure is not regression evidence')
    require(code == 0 and report.get('status') == 'passed' and
            report.get('reason') == 'admitted_studio_acceptance_completed' and
            report.get('execution_evidence') == {'frames': 4, 'edits': 1, 'undo': 1, 'redo': 1},
            'Green source-edit did not pass all existing/new assertions')
    require(not any((args.output / 'scratch').iterdir()), 'Supervised test left temporary fixture state')
    public_status(name + '-source-edit', 'verified', frames=4, edits=1, undo=1, redo=1)


def cpu_inventory(selector, code, output, cases, assertions):
    require(code == 0 and not any(marker in output for marker in PROHIBITED), selector + ' failed')
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, selector + ' has no unique pico summary')
    passed, failed, total, suites, observed_assertions = map(int, summaries[0])
    require((passed, failed, total, observed_assertions) == (cases, 0, cases, assertions),
            selector + ' case/assertion inventory differs from reviewed admission')
    return {'passed': passed, 'failed': failed, 'total': total, 'suites': suites, 'assertions': observed_assertions}


def pico_cases(code, output, names, negative_assertion=None):
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    require(not any(marker in plain for marker in (*PROHIBITED, 'ERROR: AddressSanitizer',
                'UndefinedBehaviorSanitizer', 'runtime error:')), 'Infrastructure/sanitizer failure is not assertion evidence')
    observed = re.findall(r'Running: ([A-Za-z0-9_]+)', plain)
    require(observed == names, 'Compiled selector case identity differs from reviewed workload')
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, 'Missing unique pico summary')
    passed, failed, total, suites, assertions = map(int, summaries[0])
    require(total == len(names) and passed + failed == total and assertions > 0, 'Wrong pico inventory')
    if negative_assertion is None:
        require(code == 0 and failed == 0 and '(FAILED:' not in plain, 'Candidate assertion workload failed')
    else:
        failures = [line for line in plain.splitlines() if '(FAILED:' in line]
        require(code == 1 and failed > 0 and failures and re.search(negative_assertion, failures[0]),
                'First negative failure is not its reviewed assertion')
    return {'cases': names, 'passed': passed, 'failed': failed, 'total': total,
            'suites': suites, 'assertions': assertions, 'negative_assertion': negative_assertion}


def prototype_child(arguments):
    # Fixture cloning and cleanup share the inherited production deadline with
    # this single two-frame invocation; no fresh budget for its descendant.
    import tempfile
    selector, *paths = arguments
    require(selector in ('--prototype-source', '--prototype-source-authority'), 'Unreviewed prototype selector')
    engine, source, binary, scratch = map(Path, paths)
    sys.path.insert(0, str(engine / 'tools'))
    from test_policy import run_commands
    from agent import clone_project
    with tempfile.TemporaryDirectory(prefix='prototype-', dir=scratch) as directory:
        fixture = Path(directory) / 'project'
        clone_project(source / 'tests/acceptance/project', fixture)
        return run_commands([['xvfb-run', '-a', str(binary), selector, '--project-root', str(fixture)]], seconds=60)


def prototype_markers(output, case):
    # pico emits this exact case banner without a newline. The authority
    # fixture's first stdout printf can immediately follow it on the same line.
    # The caller first verifies the unique compiled case with pico_cases.
    banner = 'Running: ' + case + ' '
    lines = [line.removeprefix(banner) for line in output.splitlines()]
    formats = (
        ('edge', 'Prototype source edge controls:',
         r'Prototype source edge controls: deleted_source_retained=(-?\d{1,10}) queued_commands_quiet=(-?\d{1,10})'),
        ('authority', 'Prototype source authority:',
         r'Prototype source authority: Code unchanged=(-?\d{1,10}), Properties unchanged=(-?\d{1,10})'),
        ('intent', 'Prototype source intent:', r'Prototype source intent: retry_held=(-?\d{1,10})'),
        ('frame', 'Prototype source frame:', r'Prototype source frame: held=(-?\d{1,10}), live=(-?\d{1,10})'),
        ('disk', 'Prototype source disk:', r'Prototype source disk: conflict=(-?\d{1,10})'),
        ('controls', 'Prototype source controls:',
         r'Prototype source controls: notice_retained=(-?\d{1,10}) focus_retained=(-?\d{1,10}) enter_edits_draft=(-?\d{1,10})'),
    )
    edges, authority, records, malformed = [], [], [], 0
    for line in lines:
        for name, prefix, pattern in formats:
            if not line.startswith(prefix):
                continue
            (edges if name == 'edge' else authority).append(line)
            match = re.fullmatch(pattern, line)
            records.append({name: list(map(int, match.groups())) if match else None})
            malformed += match is None
            break
    # Only these fixed public grammar keys and integers reach retained evidence.
    return edges, authority, {
        'markers': records[:16], 'marker_count': len(records), 'malformed_markers': malformed,
        'additional_markers': max(0, len(records) - 16),
        'normalized_banners': sum(line.startswith(banner) for line in output.splitlines()),
    }


def reuse_edge_evidence(policy_sha256):
    prior = PRIOR_EDGE_EVIDENCE
    valid = (FINAL, FINAL_TREE, ENGINE) == (prior['studio_revision'], prior['studio_tree'], prior['engine_revision'])
    valid &= canonical(EDGE_CONTROL) == prior['control_sha256']
    valid &= policy_sha256 == prior['policy_sha256']
    valid &= canonical(prior['inventory']) == 'bf85ed32c78b95a7036bd90998619c9edd8ab6cd2a4a569644e2d4bee338ed18'
    require(valid, 'Prior edge evidence no longer matches reviewed workload')
    save(PUBLIC_DIRECTORY / 'prior-edge-evidence.json', prior)
    public_status('edges-negative-reuse', 'verified-negative', reused=True, cases=1, assertions=264)


def prototype_test(args, label, source, artifact, env, authority=False):
    build, _, native, evidence = artifact
    selector = '--prototype-source-authority' if authority else '--prototype-source'
    case = 'prototype_source_authority' if authority else 'prototype_source_ownership'
    fixture_header = 'tests/acceptance/prototype_source_authority.h' if authority else 'tests/acceptance/prototype_source.h'
    success = AUTHORITY_SUCCESS if authority else PROTOTYPE_SUCCESS
    require(digest(build / 'bin/studio-acceptance') == evidence['binary_sha256'], 'Prototype binary changed')
    require(digest(build / 'compile_commands.json') == evidence['compile_commands_sha256'], 'Prototype build inputs changed')
    from agent import studio_acceptance_sources, studio_resource_preflight, external_project
    studio_acceptance_sources(source, evidence['files'])
    studio_resource_preflight(build, evidence['resources'])
    fixture = source / 'tests/acceptance/project'
    members = [p for p in fixture.rglob('*') if p.is_file()]
    require(len(members) == 3 and not any(p.is_symlink() for p in fixture.rglob('*')),
            'Prototype host fixture membership changed')
    require(external_project((fixture / 'project.kdl').read_bytes()), 'Prototype host project changed')
    expected = {
        'content/scenes/Start.kdl': b'version 1\nname "Start"\n(array)entities\n',
        'content/behaviors/Counter.chibi': b'Counter :: behavior {\n    count: retain int = 1\n    on Tick {\n        count = count + 1\n    }\n}\n'}
    require(all((fixture / p).read_bytes() == value for p, value in expected.items()), 'Prototype host source changed')
    env = dict(env)
    env.pop('CHIBI_STUDIO_CONTROLS_CAPTURE', None)
    capture = PUBLIC_DIRECTORY / 'studio-controls.png' if authority and label == 'green' else None
    if capture is not None:
        require(not capture.exists(), 'Capture output already exists')
        env['CHIBI_STUDIO_CONTROLS_CAPTURE'] = str(capture)
    code, output = supervised(args, label + selector[1:], [[sys.executable, str(Path(__file__).resolve()),
        '--prototype-child', selector, str(args.engine), str(source), str(build / 'bin/studio-acceptance'), str(args.output / 'scratch')]], env)
    red = label in CONTROLS
    control = CONTROLS.get(label)
    require((red and CONTROLS[label].get('authority', False)) == (red and authority), 'Control selector mismatch')
    inventory = pico_cases(code, output, [case], control['assertion'] if red else None)
    markers = control.get('required_markers', []) if red else ([AUTHORITY_DIAGNOSTIC, INTENT_SUCCESS, FRAME_SUCCESS, DISK_SUCCESS, CAPTURE_SUCCESS] if authority else [EDGE_SUCCESS])
    observed_edges, observed_authority, observations = prototype_markers(output, case)
    save(PUBLIC_DIRECTORY / (label + selector[1:] + '-markers.json'), observations)
    require(observed_edges == ([] if authority else markers), 'Studio edge diagnostics differ from the unique reviewed result')
    require(observed_authority == (markers if authority else []), 'Studio authority diagnostics differ from the unique reviewed result')
    require(output.splitlines().count(success) == (0 if red else 1), 'Prototype completion marker differs')
    if capture is not None:
        image = verify_capture(capture)
        save(PUBLIC_DIRECTORY / 'studio-controls.json', {**image, 'frames': 2,
             'notice_retained': True, 'focus_retained': True, 'enter_edits_draft': True,
             'visual_review': 'pending independent pixel inspection'})
    save(args.output / 'evidence' / (label + ('-authority-inventory.json' if authority else '-prototype-inventory.json')), {
        **inventory, 'selector': selector, 'frames_limit': 2,
        'success': success, 'required_control_markers': markers,
        'retry_intent_probe': 'not reached' if label == 'authority-red' else ('reached' if authority else 'not selected'),
        'binary_sha256': evidence['binary_sha256'],
        'source_sha256': evidence['files'][fixture_header]['sha256']})
    require(not any((args.output / 'scratch').iterdir()), 'Prototype fixture cleanup incomplete')
    # Opening Studio can migrate its disposable bundled project manifest. Only
    # this known input is restored before another selector uses the same build.
    shutil.copy2(source / 'projects/studio/project.kdl', build / 'share/chibi/studio/project.kdl')
    studio_resource_preflight(build, evidence['resources'])
    save(PUBLIC_DIRECTORY / (label + selector[1:] + '-inventory.json'), {
        **inventory, 'selector': selector, 'expected_diagnostics': markers,
        'status': 'verified-negative' if red else 'verified',
        'binary_sha256': evidence['binary_sha256'], 'frames_limit': 2,
    })
    public_status(label + selector[1:], 'verified-negative' if red else 'verified',
                  cases=inventory['total'], assertions=inventory['assertions'],
                  frames_limit=2, intended_failure=red)


def verify_capture(path):
    # A bounded PNG integrity check. Pixel decoding/review remains independent.
    require(path.is_file() and not path.is_symlink() and 0 < path.stat().st_size <= 24 * 1024 * 1024,
            'Screenshot file bound')
    data = path.read_bytes()
    require(data[:8] == b'\x89PNG\r\n\x1a\n', 'Screenshot is not PNG')
    offset, chunks, compressed = 8, [], bytearray()
    while offset < len(data):
        require(offset + 12 <= len(data), 'Truncated PNG chunk')
        size = int.from_bytes(data[offset:offset + 4], 'big')
        kind = data[offset + 4:offset + 8]
        end = offset + 8 + size
        require(end + 4 <= len(data), 'PNG chunk exceeds file')
        payload = data[offset + 8:end]
        require(zlib.crc32(kind + payload) & 0xffffffff == int.from_bytes(data[end:end + 4], 'big'),
                'Screenshot chunk checksum differs')
        chunks.append(kind)
        if kind == b'IHDR':
            require(len(chunks) == 1 and size == 13, 'Unexpected screenshot header')
            width, height = int.from_bytes(payload[:4], 'big'), int.from_bytes(payload[4:8], 'big')
            depth, color, compression, filtering, interlace = payload[8:]
            require((width, height, depth, compression, filtering, interlace) == (1920, 1080, 8, 0, 0, 0)
                    and color in (2, 6), 'Screenshot format differs from reviewed canvas')
        elif kind == b'IDAT':
            compressed.extend(payload)
        elif kind == b'IEND':
            require(size == 0 and end + 4 == len(data), 'Unexpected PNG end')
        else:
            require(kind[0] & 32, 'Unsupported critical screenshot chunk')
        offset = end + 4
    require(chunks and chunks[0] == b'IHDR' and chunks[-1] == b'IEND' and compressed,
            'Screenshot lacks complete image')
    stride = width * (3 if color == 2 else 4) + 1
    decoder = zlib.decompressobj()
    pixels = decoder.decompress(compressed, stride * height + 1)
    require(len(pixels) == stride * height and decoder.eof and not decoder.unused_data and
            not decoder.unconsumed_tail and all(pixels[i] <= 4 for i in range(0, len(pixels), stride)),
            'Screenshot image data did not decode completely')
    return {'sha256': digest(path), 'width': width, 'height': height, 'image_stream_verified': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'engine', 'sdk', 'validation', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('source', 'engine', 'sdk', 'validation', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    global PUBLIC_DIRECTORY
    args.output.mkdir(exist_ok=False)
    PUBLIC_DIRECTORY = args.output / 'public'
    for directory in ('logs', 'evidence', 'public'):
        (args.output / directory).mkdir()
    os.chdir(args.engine)
    sys.path.insert(0, str(args.engine / 'tools'))
    policy_hashes = snapshot(args.engine, POLICY)
    save(PUBLIC_DIRECTORY / 'policy-identity.json', {'policy_sha256': canonical(policy_hashes),
         'supervisor_sha256': policy_hashes['tools/test_policy.py'], 'seconds_limit': 60})
    controls, final_files = source_inputs(args)
    reuse_edge_evidence(canonical(policy_hashes))
    require(ENGINE == PRIOR_ENGINE_EVIDENCE['engine_revision'], 'Prior engine evidence does not cover this pin')
    save(args.output / 'evidence/prior-engine-evidence.json', PRIOR_ENGINE_EVIDENCE)
    save(args.output / 'evidence/prior-ownership-evidence.json', PRIOR_OWNERSHIP_EVIDENCE)
    sdk = sdk_identity(args)
    env = graphics_environment(args)
    control_builds = []
    for label, source in controls.items():
        if label == 'edges-red':
            continue  # Exact successful prior evidence was checked above; do not rebuild/rerun.
        artifact = build_variant(args, label, source, sdk)
        control_builds.append(artifact[0])
        prototype_test(args, label, source, artifact, env, authority=CONTROLS[label].get('authority', False))
    green_artifact = build_variant(args, 'green', args.source, sdk)
    prototype_test(args, 'green', args.source, green_artifact, env)
    prototype_test(args, 'green', args.source, green_artifact, env, authority=True)
    gpu_test(args, 'green', args.source, green_artifact, env)
    binary = green_artifact[0] / 'bin/studio-acceptance'
    policy_source = args.source / 'projects/studio/content/libraries/OutlinerRowPolicies.chibi'
    for selector, extra, cases, assertions in (
            ('--project-open', [], 1, 50), ('--scene-read', [], 6, 1141),
            ('--outliner-row-policies', ['--source', str(policy_source)], 1, 1848)):
        require(digest(binary) == green_artifact[3]['binary_sha256'], 'Green binary changed before CPU selector')
        code, output = supervised(args, selector[2:], [[str(binary), selector, *extra]], env)
        inventory = cpu_inventory(selector, code, output, cases, assertions)
        save(args.output / 'evidence' / (selector[2:] + '-inventory.json'), inventory)
        public_status(selector[2:], 'verified', cases=cases, assertions=assertions)
    stage('final-integrity')
    require(snapshot(args.source, final_files, SOURCE_LINKS) == final_files,
            'Final source changed during validation')
    clean_checkout(args.source, FINAL)
    require(git(args.source, 'rev-parse', 'HEAD^{tree}') == FINAL_TREE, 'Reviewed Studio tree differs')
    clean_checkout(args.engine, ENGINE)
    require(snapshot(args.engine, POLICY) == policy_hashes, 'Production admission/supervisor changed')
    require(sdk_identity(args) == sdk, 'Installed SDK changed during validation')
    require(not any((args.output / 'scratch').iterdir()), 'Temporary fixture cleanup incomplete')
    save(PUBLIC_DIRECTORY / 'summary.json', {
        'passed': True, 'scope': 'Studio-only exact-head validation',
        'engine_checks_reused': True, 'ownership_negative_reused': True,
        'edge_negative_reused': True,
        'new_negative_controls': len(CONTROLS) - 1, 'clean_prototype_selectors': 2,
        'source_edit': {'frames': 4, 'edits': 1, 'undo': 1, 'redo': 1},
        'cpu': {'cases': 8, 'assertions': 3039}, 'seconds_limit_per_invocation': 60,
        'full_suite': 'not run', 'browser': 'not run',
        'rendered_controls': 'captured; independent visual review pending',
        'logging': 'fixed statuses and verified counts only; raw output ephemeral',
    })
    for path in (*controls.values(), *control_builds, green_artifact[0], args.output / 'scratch',
                 args.output / 'cache', args.output / 'prefs'):
        shutil.rmtree(path)
    public_status('validation', 'verified')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 7 and sys.argv[1] == '--prototype-child':
            sys.exit(prototype_child(sys.argv[2:]))
        main()
    except Exception as error:
        # SDK/driver/compiler exceptions can contain private paths or source text.
        # Keep those in ephemeral captured output, never in public Actions logs.
        failure_status(error)
        sys.exit(1)
