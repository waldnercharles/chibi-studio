#!/usr/bin/env python3
"""Temporary exact-head Studio PR77 validation only; never merge into product."""
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
import tempfile

sys.dont_write_bytecode = True
# Binding requires the coordinator's actual published integrated commit and review.
FINAL = '854d19acad786b6c9640002597a3f98a7f3ca791'
FINAL_TREE = '536ed611531de9e2f9e055f7541d6a442e4e0523'
ENGINE = '7bc6aa3cd69b29cfb006aab94d50d695007d9abe'
SOURCE_PATCH_SHA256 = '3e8aeb7cd0c0c486e98ea9af9f9b8c363bc165b0659ce9d71a07d3f323fce90d'
PUBLIC_DIRECTORY = None
CURRENT_STAGE = 'startup'
PRODUCT = 'src/editor/rmlui/kit_code.c'
INFRA = {'.github/workflows/ci.yml', 'tools/validate_studio_refresh.py'}
POLICY = ('tools/agent.py', 'tools/test_policy.py', 'test/execution_selectors.h')
PROHIBITED = ('Studio UI:', 'runtime frame failed', 'Could not initialize',
              'ERROR: AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:')
WORKLOAD = {
    '--graph-visibility': {'case': 'graph_visibility_and_document_pair', 'frames': 10,
        'agent': '--studio-graph-visibility',
        'success': 'Studio graph visibility accepted: frames=10'},
    '--graph-pointer': {'case': 'graph_pointer', 'frames': 19,
        'agent': '--studio-graph-pointer',
        'success': 'Studio graph pointer accepted: frames=19'},
    '--source-edit': {'case': 'source_graph_edit_undo_redo', 'frames': 4,
        'agent': '--studio-source-edit',
        'success': 'Studio source edit accepted: frames=4 edits=1 undo=1 redo=1'},
    '--code-wrap-reveal': {'case': 'code_wrap_retargets_reveal', 'frames': 5,
        'success': 'Studio code wrap reveal accepted: frames=5', 'assertions': 69},
}
WRAP_ASSERTION = r'main\.c \(1617\): x == 0\)'
WRAP_FIXTURE_SHA256 = '960d9b1db417b960387eb07b1e3389e4e18160d3a17dde71131c5065a6b7dd43'
FAILURE_REASONS = {'Acceptance binary changed': 'binary_identity_mismatch',
 'Acceptance build inputs changed': 'build_identity_mismatch',
 'Acceptance fixture cleanup incomplete': 'fixture_cleanup_failed',
 'Acceptance source changed': 'source_identity_mismatch',
 'Candidate assertion workload failed': 'candidate_assertion_failure',
 'Compiled selector case identity differs from reviewed workload': 'case_inventory_mismatch',
 'Counterfactual source differs': 'counterfactual_source_mismatch',
 'Counterfactual writer differs from published Git blob': 'mutation_git_blob_mismatch',
 'Final reviewed Studio SHA has not been supplied': 'unbound_product_head',
 'First negative failure is not its reviewed assertion': 'unexpected_first_assertion',
 'Infrastructure/sanitizer failure is not assertion evidence': 'infrastructure_or_sanitizer_failure',
 'Installed SDK changed during validation': 'sdk_identity_mismatch',
 'Missing link-time debug stripping': 'studio_build_configuration_mismatch',
 'Missing unique pico summary': 'missing_or_duplicate_case_summary',
 'Mutation bytes differ from review': 'mutation_hash_mismatch',
 'Mutation requires exactly one reviewed original span': 'mutation_span_mismatch',
 'Owning selector report differs': 'selector_report_mismatch',
 'Private engine source compiled': 'installed_sdk_boundary_violation',
 'Production admission/supervisor changed': 'policy_identity_mismatch',
 'Reviewed Studio tree differs': 'studio_tree_mismatch',
 'SDK source is not the exact clean engine pin': 'sdk_revision_mismatch',
 'Studio assertions disabled': 'studio_assertions_disabled',
 'Validation child changes files outside its two infrastructure files': 'validation_scope_mismatch',
 'Wrap assertion count differs': 'assertion_count_mismatch',
 'Wrap completion marker differs': 'completion_marker_mismatch',
 'Wrong pico inventory': 'case_count_mismatch'}
SOURCE_LINKS = {'CLAUDE.md': {'type': 'symlink', 'git_mode': '120000', 'target': 'AGENTS.md', 'git_blob': '47dc3e3d863cfb5727b87d785d09abf9743c0a72'}}
REVIEWED_FILES = {'.github/engine-ref': '0aa450a62d7457af7dab66b47d1555806707bbd621c6d7a2c909dfb4fe967875',
 'EXTRACTION.json': '49e5bb887001488ba95318c05911389b5ce29322c8b2acdbebfb6843ea1fe140',
 'SDK_HEADERS.json': '56e445f1f418a6f5e838ecb05645748c40167e3ec5a146871e78ddedb02d874b',
 'cmake/patches/zep-smooth-reveal.patch': '0daf6f5f1291b8fb293e1c164ea3191308d839a3ae77f473e1e5065f8976af0b',
 'cmake/studio_sources.cmake': '02705e472a50898a7068f5f7993c7479870ab1e247eaa3a1cd116be5faa35aca',
 'cmake/zep_dependencies.cmake': '14e2c0b03fed9990cb51746386c315ac76b8fcba5a59fc5cfb64c225193cf0d0',
 'src/editor/collab/actions_scene.c': 'ccd7e001355c6b1e00641b3190e995f951ded077c77d6569743d91f312ca6d6e',
 'src/editor/collab/collab.c': 'af5959e30bc7ec83da31547d9d1d2ec1169df84bf70878d60209e3df8ec2a353',
 'src/editor/collab/collab.h': '5ad2e237b948e3818ea595e9f38efb0557e51dee09f190c2bd176fc4f6023d4b',
 'src/editor/collab/protocol.h': 'd30ffb61a1d099eb5d7399d9f3f09020c94cce0476bf758ee7ce4c1b9fd1e92d',
 'src/editor/documents/asset_code.c': '87ffc2770a876ae48da68f2f979f2470e6effc238a90df4e0e0f0f1435f4103d',
 'src/editor/documents/asset_code.h': '012f40bdfc5a1d5b5d71aa9c2f6a6ff988c154b4b83b4890316283ac76400201',
 'src/editor/documents/prototype_source_input.c': '58a2f0c5d921760f6aa0529a10d6ec507f84214778a5b04cbaa42e2872ad38f6',
 'src/editor/documents/prototype_source_input.h': '76772f93dc0458baf0695b85f7851c9a521c19bb8b4afc0d0f52edf3c4668971',
 'src/editor/documents/scene_disk.c': '61df3e4276a2d24f311727aea2056a5e314a15f6f3ea9eb0366f320e78db5e65',
 'src/editor/documents/scene_documents.c': '829844d7c5f026835c33b608f2a4611586bf8b87959674a5586ca2c0a87acee2',
 'src/editor/documents/scene_history.c': 'c85f0c20e977bb1e679a6904b0812eee01e148b2f1390469f6a509355bccc7ab',
 'src/editor/documents/scene_history.h': 'b4ff9050dd63cb59ad410efb7aa9a384eca64ff549766597535a8eba2e63946d',
 'src/editor/documents/scene_prototype_actions.c': '02184147bc3e50eecf4703520ca13435f1128b1f95f9a4c498fbf1a908cabba3',
 'src/editor/documents/scene_prototypes.c': '1207d742b108ed245272481b29ed3e2a06757e8234f8eec098f4b8300b92f92e',
 'src/editor/rmlui/kit_code.c': '2e2715e0ef9f0a6154d52e9bfb846ba912717283c7a8373da2489043d9abdabf',
 'src/editor/studio/studio_code.c': '18572ebe982bdd02df988fca471c32a864789927a0fadcf68509169b1883a23b',
 'src/editor/studio/studio_graph.c': '53e3b9fb76c97dbfc3fc3cbaf20e0dc89cdf616ac15c5a33d30f5d46ec83dfc3',
 'src/editor/studio/studio_graph_draw.c': 'ae118e30a9996df883acdacdfb160e596cd18e0dfed80c6f9acbb204c337051a',
 'src/editor/studio/studio_graph_input.c': '44ee05974e6086c5986bbb0c01f2d529d11df6ab77e2bfbabfa23498b9be2a9e',
 'src/editor/studio/studio_graph_internal.h': '0f358500fd2053fbb01e2ba8da119f6d6746c625d8786bbab373a051af6bece9',
 'src/editor/studio/studio_graph_view.c': '0291cbda2fbfb5390fbb4a62010942e0b6ff073039e240475ff57cc398757280',
 'src/editor/studio/studio_internal.h': '8fc637f842f428c785525eb014d221caefa35e6e7d21436511aa3139a7ae37a8',
 'src/editor/studio/studio_projects.c': '4c02c4fab9462e86aa6ef40f2d3cd75842cbc4c69dade6d43d3d5e422b083a47',
 'src/editor/studio/studio_properties.c': 'd20368e746e6070e7fdc8942c5e73d5504f89ffafd62e107998a4bf4e45e34a4',
 'src/editor/studio/studio_properties_view.c': '728ddaa0c20b1806bf0a8e375cd2c39b8bf6a2f3f63bb5c9cede856ac40374cb',
 'src/editor/studio/studio_prototype_actions.c': '61f768872d015bb3d9ea9e140a077434b86cb5e29e88607cd408fd730fb5373f',
 'src/editor/studio/studio_shell.c': '04bf2ee8660e9d4fe10c6deac391eea6a9b5315cabe7b653a369fbc929dd5202',
 'src/editor/text/zep_bridge.cpp': '7d8768702ce3295eb96fe043f9f5881edc444da0bd59b50476ee11fbd7818bc8',
 'src/editor/text/zep_bridge.h': 'b8584de1a28e295f9270fcdc8157f15323cf970426a472bac8d12c557d66bcb7',
 'src/editor/ui/kit.h': '2391a71b42ce28ab80cefd41aeb38232234018d32106e5376fe9c1bb76fef11c',
 'src/editor/workspace/scene_actors.c': '2e18d5ca42dcfb77671a0937bca2d0018f865239e27db13dfdac2caa4296f9b4',
 'src/editor/workspace/scene_authoring.c': 'd7fad2e23281b7612c7bb43780f4823b0bc8edc6f0a2e47cf364bebc6f45977e',
 'src/editor/workspace/scene_authoring.h': '73d7890233f253eca58066ce9aa1b44b4ac2c47f9a30a4ec42ff457dd89e3c47',
 'src/editor/workspace/scene_checkpoint.c': '3a80436d6f4d135264549f018c95fbb83e1c6e5fa5abf646ccb743be58aa7581',
 'src/editor/workspace/scene_internal.h': 'ce497c867a2fe4419801d1e7b2cc54c66bafd4f8f3f4f537e41139348d30ce9f',
 'src/editor/workspace/scene_tabs.c': '8c4f04ed722496c82b54b924d7570a04ccc0d5408dc6931a08bc9a97d5e44131',
 'tests/acceptance/README.md': '311de373f8eb291d708a4bf5af2dcd21cfe311a61adea2dff2f9bf0a8a9e984a',
 'tests/acceptance/main.c': '87838de028236ee7cf3204cfaabfd2c495aa147c0e7ac704310848ea586e94db',
 'tests/acceptance/project/content/behaviors/Counter.chibi': '8ac097335341b83e44da77f9a8f90adfd23a4b71114aa014bfe7a620e95e24e4',
 'tests/acceptance/project/content/scenes/Start.kdl': '4512130a3539013e0ec2e50c0c0427cb9f027f9f997d95379b6a47950aee745b',
 'tests/acceptance/project/project.kdl': '6ddf1d450404852e6872e95110e048dc05a439d0646bab7d6b60cf30cf1ae68e',
 'tests/acceptance/prototype_source.h': '61c08b44b025cd63494b5c9097e2d35f928c2e8d3dc16e1f32ec4b45e9f9688f',
 'tests/acceptance/prototype_source_authority.h': '4bf10c84b6b2902bd0ab5f4ff797bd980ba7918b679bf526e2075c157d3e946a',
 'tests/acceptance/prototype_source_capture.h': '9efc027d65b1f4e44b2269c09abe8439900c7224914a8c22bfd9c4b65eb25807',
 'tests/acceptance/scene_move.h': 'cb1281598c00ce449f070d3ca4a69983f6c7645656052f64e330c5f0b62f525d',
 'tests/acceptance/source_action_outcomes.h': 'efdb4571214e15a104428f68e0fcf8b9835e222264908fca076a79898a098f32',
 'tests/acceptance/source_discard.h': '38eb2fb91b2385b8e98e0694e78fda83ee68e7ab62979712169663c5af55187b'}
WRAP_MUTATION = {'after': '',
 'before': '    bool reveal = e->reveal_pending || e->revealing;\n'
           '    cancel_reveal(e);\n'
           '    e->reveal_pending = reveal;\n',
 'git_blob': 'fd519aa930b1d029feafb49b7371fce66b866cd2',
 'sha256': 'c2e05e30f4f0fab4b16140cb3f1cb062eed9b173160440088206a952250dd090'}


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
    build = args.output / 'studio-build'
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
                'hidden': True, 'window': [1920, 1080],
                'files': files, 'resources': resources, 'candidate_revision': FINAL,
                'counterfactual': name == 'wrap-red', 'source_dirty': name == 'wrap-red', 'variant': name,
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
    for directory in ('scratch', 'cache'):
        (args.output / directory).mkdir()
    env.update(VK_DRIVER_FILES=str(icds[0]), SDL_GPU_DRIVER='vulkan', SDL_VIDEO_DRIVER='x11',
               SDL_AUDIODRIVER='dummy', SDL_LOGGING='gpu=info', TMPDIR=str(args.output / 'scratch'),
               XDG_CACHE_HOME=str(args.output / 'cache'), MESA_SHADER_CACHE_DIR=str(args.output / 'cache'))
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

def pico_cases(code, output, names, negative_assertion=None):
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    require(not any(marker in plain for marker in (*PROHIBITED, 'ERROR: AddressSanitizer',
                'UndefinedBehaviorSanitizer', 'runtime error:')), 'Infrastructure/sanitizer failure is not assertion evidence')
    observed = re.findall(r'Running: ([A-Za-z0-9_]+)', plain)
    require(observed == names, 'Compiled selector case identity differs from reviewed workload')
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, 'Missing unique pico summary')
    passed, failed, total, suites, assertions = map(int, summaries[0])
    require(total == len(names) and passed + failed == total and suites == 1 and assertions > 0,
            'Wrong pico inventory')
    if negative_assertion is None:
        require(code == 0 and failed == 0 and '(FAILED:' not in plain, 'Candidate assertion workload failed')
    else:
        failures = [line for line in plain.splitlines() if '(FAILED:' in line]
        require(code == 1 and failed > 0 and failures and re.search(negative_assertion, failures[0]),
                'First negative failure is not its reviewed assertion')
    return {'cases': names, 'passed': passed, 'failed': failed, 'total': total,
            'suites': suites, 'assertions': assertions, 'negative_assertion': negative_assertion}

def reviewed_pins():
    for name, value in (('Studio', FINAL), ('Studio tree', FINAL_TREE), ('engine', ENGINE)):
        require(re.fullmatch(r'[0-9a-f]{40}', value), 'Final reviewed ' + name + ' SHA has not been supplied')
    require(set(WORKLOAD) == {'--graph-visibility', '--graph-pointer', '--source-edit', '--code-wrap-reveal'},
            'Workload is not frozen')
    require(REVIEWED_FILES and re.fullmatch(r'[0-9a-f]{64}', WRAP_MUTATION['sha256']),
            'Reviewed source identities have not been supplied')


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
    source_names = tracked(args.source)
    final_files = snapshot(args.source, source_names, SOURCE_LINKS)
    validation_files = snapshot(args.validation, tracked(args.validation), SOURCE_LINKS)
    require({k: v for k, v in final_files.items() if k not in INFRA} ==
            {k: v for k, v in validation_files.items() if k not in INFRA},
            'Validation child changes files outside its two infrastructure files')
    require(digest(Path(__file__).resolve()) == validation_files['tools/validate_studio_refresh.py'],
            'Running helper differs from validation checkout')
    manifest = extraction(args.source)
    for name in REVIEWED_FILES.keys() & manifest['files'].keys():
        require(manifest['files'][name].get('consumer_sha256') == digest(args.source / name),
                'Edited extracted file lacks current consumer checksum: ' + name)
    # One disposable source location permits a genuinely incremental one-file
    # counterfactual rebuild after the one clean build. The published tree stays
    # untouched. All four clean invocations finish before the mutation.
    work = args.output / 'product-source'
    work.mkdir()
    for name in source_names:
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.source / name, target, follow_symlinks=False)
    require(snapshot(work, source_names, SOURCE_LINKS) == final_files, 'Acceptance source changed')
    save(PUBLIC_DIRECTORY / 'identity.json', {
        'studio_revision': FINAL, 'studio_tree': FINAL_TREE,
        'source_patch_sha256': SOURCE_PATCH_SHA256,
        'validation_revision': git(args.validation, 'rev-parse', 'HEAD'),
        'helper_sha256': digest(Path(__file__).resolve()),
        'workflow_sha256': digest(args.validation / '.github/workflows/ci.yml'),
        'engine_revision': ENGINE, 'engine_checks_reused': True,
        'studio_reviewed_files': REVIEWED_FILES,
    })
    save(args.output / 'evidence/source-identity.json', {'final_files': final_files})
    return work, final_files


def apply_counterfactual(work, final_files):
    require(snapshot(work, final_files, SOURCE_LINKS) == final_files, 'Acceptance source changed')
    (work / PRODUCT).write_text(mutate((work / PRODUCT).read_text(), WRAP_MUTATION))
    manifest = json.loads((work / 'EXTRACTION.json').read_text())
    manifest['files'][PRODUCT]['consumer_sha256'] = WRAP_MUTATION['sha256']
    save(work / 'EXTRACTION.json', manifest)
    require(extraction(work) == manifest, 'Studio counterfactual extraction audit failed')
    red_files = snapshot(work, final_files, SOURCE_LINKS)
    require({name for name in final_files if red_files[name] != final_files[name]} ==
            {PRODUCT, 'EXTRACTION.json'}, 'Counterfactual source differs')
    return red_files


def verify_artifact(source, artifact):
    build, _, _, evidence = artifact
    require(digest(build / 'bin/studio-acceptance') == evidence['binary_sha256'], 'Acceptance binary changed')
    require(digest(build / 'compile_commands.json') == evidence['compile_commands_sha256'],
            'Acceptance build inputs changed')
    require(digest(source / 'EXTRACTION.json') == evidence['extraction_sha256'], 'Acceptance source changed')
    require(digest(source / PRODUCT) == evidence['product_sha256'], 'Acceptance source changed')
    from agent import studio_acceptance_sources, studio_resource_preflight, external_project
    studio_acceptance_sources(source, evidence['files'])
    studio_resource_preflight(build, evidence['resources'])
    fixture = source / 'tests/acceptance/project'
    files = {p.relative_to(fixture).as_posix() for p in fixture.rglob('*') if p.is_file()}
    expected = {'project.kdl', 'content/scenes/Start.kdl', 'content/behaviors/Counter.chibi'}
    require(files == expected and not any(p.is_symlink() for p in fixture.rglob('*')),
            'Acceptance fixture membership changed')
    for name in expected:
        require(digest(fixture / name) == REVIEWED_FILES['tests/acceptance/project/' + name],
                'Acceptance fixture changed')
    require(external_project((fixture / 'project.kdl').read_bytes()), 'Acceptance fixture changed')


def restore_resources(source, artifact):
    build, _, _, evidence = artifact
    # The real host may migrate its disposable bundled project manifest.
    # Restore only that known input, then require every resource hash again.
    shutil.copy2(source / 'projects/studio/project.kdl', build / 'share/chibi/studio/project.kdl')
    from agent import studio_resource_preflight
    studio_resource_preflight(build, evidence['resources'])
    verify_artifact(source, artifact)


def owning_report(selector, code, output):
    row = WORKLOAD[selector]
    require('agent' in row, 'Unreviewed owning selector')
    reports = []
    for line in output.splitlines():
        try:
            report = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(report, dict) and report.get('coverage') == 'focused-iteration':
            reports.append(report)
    expected = {'frames': row['frames']}
    if selector == '--source-edit':
        expected.update(edits=1, undo=1, redo=1)
    require(not any(marker in output for marker in PROHIBITED),
            'Infrastructure/sanitizer failure is not assertion evidence')
    require(len(reports) == 1 and code == 0 and reports[0].get('status') == 'passed' and
            reports[0].get('reason') == 'admitted_studio_acceptance_completed' and
            reports[0].get('selector') == row['agent'] and
            reports[0].get('execution_evidence') == expected,
            'Owning selector report differs')
    return expected


def owning_test(args, selector, source, artifact, base_env):
    row = WORKLOAD[selector]
    require('agent' in row, 'Unreviewed owning selector')
    build, _, native, evidence = artifact
    verify_artifact(source, artifact)
    record = args.engine / '.chibi/architecture/external-studio-native-acceptance.json'
    record.parent.mkdir(parents=True, exist_ok=True)
    save(record, native)
    selected = dict(evidence, frames=row['frames'], success=row['success'])
    path = args.output / 'evidence' / (selector[2:] + '-acceptance.json')
    save(path, selected)
    env = dict(base_env, CHIBI_STUDIO_SOURCE=str(source), CHIBI_STUDIO_BUILD=str(build),
               CHIBI_STUDIO_ACCEPTANCE_EVIDENCE=str(path))
    code, output = supervised(args, 'green-' + selector[2:], [[
        'xvfb-run', '-a', sys.executable, 'tools/agent.py', 'gpu-test', '--', row['agent']]], env)
    counts = owning_report(selector, code, output)
    require(not any((args.output / 'scratch').iterdir()), 'Acceptance fixture cleanup incomplete')
    restore_resources(source, artifact)
    save(PUBLIC_DIRECTORY / ('green-' + selector[2:] + '-inventory.json'), {
        'selector': selector, 'case': row['case'], 'status': 'verified', **counts,
        'binary_sha256': evidence['binary_sha256'], 'assertion_count': 'not exported by owning gate',
    })
    public_status('green-' + selector[2:], 'verified', **counts)


def wrap_child(arguments):
    # Setup, Xvfb, the five-frame invocation, and cleanup inherit one production
    # deadline. This route adds no policy selector and never changes engine code.
    reviewed_pins()
    engine, source, binary, scratch = map(Path, arguments)
    sys.path.insert(0, str(engine / 'tools'))
    from test_policy import run_commands, DEADLINE
    from agent import clone_project
    require(DEADLINE in os.environ, 'Missing inherited production deadline')
    with tempfile.TemporaryDirectory(prefix='wrap-', dir=scratch) as directory:
        fixture = Path(directory) / 'project'
        clone_project(source / 'tests/acceptance/project', fixture)
        env = dict(os.environ, CHIBI_STUDIO_PREFS_PATH=str(fixture / '.acceptance-prefs.json'),
                   CHIBI_THUMBNAIL_CACHE=str(fixture / '.acceptance-thumbnails'))
        return run_commands([['xvfb-run', '-a', str(binary), '--code-wrap-reveal',
                              '--project-root', str(fixture)]], seconds=60, env=env)


def wrap_inventory(code, output, negative):
    row = WORKLOAD['--code-wrap-reveal']
    inventory = pico_cases(code, output, [row['case']], WRAP_ASSERTION if negative else None)
    require(output.splitlines().count(row['success']) == (0 if negative else 1),
            'Wrap completion marker differs')
    if not negative:
        require(inventory['assertions'] == row['assertions'], 'Wrap assertion count differs')
    # The precise first assertion proves the negative reaches frame 4's x==0
    # check. Earlier parse/focus/advancing-offset failures cannot satisfy it.
    return inventory


def wrap_test(args, label, source, artifact, env):
    verify_artifact(source, artifact)
    build, _, _, evidence = artifact
    code, output = supervised(args, label + '-code-wrap-reveal', [[
        sys.executable, str(Path(__file__).resolve()), '--wrap-child', str(args.engine),
        str(source), str(build / 'bin/studio-acceptance'), str(args.output / 'scratch')]], env)
    require(label in ('green', 'wrap-red'), 'Unreviewed wrap variant')
    negative = label == 'wrap-red'
    inventory = wrap_inventory(code, output, negative)
    require(not any((args.output / 'scratch').iterdir()), 'Acceptance fixture cleanup incomplete')
    restore_resources(source, artifact)
    # Assertion expression/pattern belongs only to private local parsing.
    inventory.pop('negative_assertion')
    save(PUBLIC_DIRECTORY / (label + '-wrap-inventory.json'), {
        **inventory, 'selector': '--code-wrap-reveal', 'frames_limit': 5,
        'status': 'verified-negative' if negative else 'verified',
        'intended_failure_line': 1617 if negative else None,
        'binary_sha256': evidence['binary_sha256'],
        'fixture_bytes': 609, 'fixture_sha256': WRAP_FIXTURE_SHA256,
    })
    public_status(label + '-code-wrap-reveal', 'verified-negative' if negative else 'verified',
                  cases=1, assertions=inventory['assertions'], frames_limit=5)


def main():
    # This check runs before checkout access, build, discovery or fixture work.
    reviewed_pins()
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
    work, final_files = source_inputs(args)
    sdk = sdk_identity(args)
    env = graphics_environment(args)
    green = build_variant(args, 'green', work, sdk)
    for selector in ('--graph-visibility', '--graph-pointer', '--source-edit'):
        owning_test(args, selector, work, green, env)
    wrap_test(args, 'green', work, green, env)
    require(snapshot(work, final_files, SOURCE_LINKS) == final_files, 'Acceptance source changed')
    red_files = apply_counterfactual(work, final_files)
    # Same source/build directory, same fixture and SDK. Only kit_code.c and
    # its extraction checksum changed; CMake/Ninja can reuse unchanged objects.
    red = build_variant(args, 'wrap-red', work, sdk)
    wrap_test(args, 'wrap-red', work, red, env)
    stage('final-integrity')
    require(snapshot(work, final_files, SOURCE_LINKS) == red_files, 'Counterfactual source differs')
    require(snapshot(args.source, final_files, SOURCE_LINKS) == final_files, 'Acceptance source changed')
    clean_checkout(args.source, FINAL)
    require(git(args.source, 'rev-parse', 'HEAD^{tree}') == FINAL_TREE, 'Reviewed Studio tree differs')
    clean_checkout(args.engine, ENGINE)
    require(snapshot(args.engine, POLICY) == policy_hashes, 'Production admission/supervisor changed')
    require(sdk_identity(args) == sdk, 'Installed SDK changed during validation')
    require(not any((args.output / 'scratch').iterdir()), 'Acceptance fixture cleanup incomplete')
    save(PUBLIC_DIRECTORY / 'summary.json', {
        'passed': True, 'scope': 'Studio PR77 focused native validation',
        'engine_checks_reused': True, 'invocations': 5, 'frames_limit_total': 43,
        'clean_selectors': ['--graph-visibility', '--graph-pointer', '--source-edit', '--code-wrap-reveal'],
        'negative_selector': '--code-wrap-reveal', 'negative_first_failure_line': 1617,
        'seconds_limit_per_invocation': 60, 'clean_builds': 1, 'incremental_counterfactual_builds': 1,
        'full_suite': 'not run', 'browser': 'not run', 'web_artifact': 'not built or uploaded',
        'logging': 'fixed statuses and sanitized public Studio locations; raw output ephemeral',
    })
    for path in (work, green[0], args.output / 'scratch', args.output / 'cache'):
        shutil.rmtree(path)
    public_status('validation', 'verified')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 6 and sys.argv[1] == '--wrap-child':
            sys.exit(wrap_child(sys.argv[2:]))
        main()
    except Exception as error:
        failure_status(error)
        sys.exit(1)
