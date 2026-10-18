#!/usr/bin/env python3
"""Temporary PR71 evidence job; run only on the reviewed hosted validation PR."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
FINAL = 'a5dc980a8d066288c23c55b349d68528c5a13a95'
BASELINE = '95ca95bc3ca2da5b899ec06596982ade26d2d94f'
ENGINE = 'ff6d06c888fe660894776255ec7f9fc507db6393'
PRODUCT = 'src/editor/studio/studio_shell.c'
DISCARD_PRODUCT = 'src/editor/studio/studio_code.c'
BASELINE_HASH = '0d193b285b64f614b8094fb13c30c6988a39534af359fde76933b738f1cdc6ce'
SUCCESS = 'Studio source edit accepted: frames=4 edits=1 undo=1 redo=1'
REVIEWED_FILES = {
    'EXTRACTION.json': '4f7564b91bf1ebbc36b1fb27a8b1a06505d689962cf8cb5db481b403a0f778a6',
    'src/editor/studio/studio_code.c': 'fa25dfdf5515461d9a70cf677ee821ebb5d0fc7ed9610f3af0d9ef81033279bc',
    'src/editor/studio/studio_shell.c': 'd7a9d60016386326d193b0141c7d23d73a001afb06fbd299f33425dd93565eb7',
    'tests/acceptance/README.md': '9c00e19661d9d7d51534c108c21f13af71a32f58bb52c2d8cef6479a6ae61347',
    'tests/acceptance/main.c': '0ae8cccdd3e3eda6403d7bf9b6e84cd41f1dfd39a77e2307f3861c9b3ce148f2',
    'tests/acceptance/scene_move.h': '9a84ba7af42c74f384f0bd4db432b4d29aad66a580317c22a3f35bcdb29e07f2',
    'tests/acceptance/source_discard.h': '38eb2fb91b2385b8e98e0694e78fda83ee68e7ab62979712169663c5af55187b',
}
CONTROLS = {
    'save-red': {'product': PRODUCT, 'revision': BASELINE, 'baseline': 'baseline',
                 'sha256': BASELINE_HASH,
                 'markers': ('Static source save UndoTargets: clean=0',
                             'Static source save UndoEnemy: clean=0'),
                 'assertion': r'scene_move\.h \(380\): clean_saves\)'},
    'discard-red': {'product': DISCARD_PRODUCT, 'revision': BASELINE, 'baseline': 'baseline',
                    'sha256': '4f659262c6a6e9b12c2a97044b3d6933218187b3642381ad135c8d4f6da750ca',
                    'markers': ('Source-only executable discard cleanup complete: cases=6 passed=0 ordinary=1/1',),
                    'assertion': r'source_discard\.h \(185\): all_valid\)'},
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


def git(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments], text=True).strip()


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
    for name in (PRODUCT, DISCARD_PRODUCT):
        require(name in value['files'] and 'consumer_sha256' in value['files'][name],
                'Missing edited-product consumer checksum: ' + name)
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


def source_inputs(args):
    require(re.fullmatch(r'[0-9a-f]{40}', FINAL), 'Final reviewed Studio SHA has not been supplied')
    for path, revision in ((args.source, FINAL), (args.baseline, BASELINE),
                           (args.engine, ENGINE)):
        clean_checkout(path, revision)
    require((args.source / '.github/engine-ref').read_text().strip() == ENGINE, 'Engine pin changed')
    require(REVIEWED_FILES, 'Reviewed source identities have not been supplied')
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
    final_manifest = extraction(args.source)
    controls, records = {}, {}
    for label, control in CONTROLS.items():
        product = control['product']
        baseline = getattr(args, control['baseline'])
        require(digest(baseline / product) == control['sha256'], 'Wrong old product: ' + label)
        red = args.output / (label + '-source')
        red.mkdir()
        for name in source_names:
            target = red / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source / name, target, follow_symlinks=False)
        shutil.copy2(baseline / product, red / product)
        manifest = json.loads(json.dumps(final_manifest))
        manifest['files'][product]['consumer_sha256'] = control['sha256']
        save(red / 'EXTRACTION.json', manifest)
        require(extraction(red) == manifest, 'Red extraction audit failed: ' + label)
        expected_manifest = json.loads(json.dumps(final_manifest))
        expected_manifest['files'][product]['consumer_sha256'] = control['sha256']
        require(manifest == expected_manifest, 'Red changed unrelated extraction metadata')
        red_files = snapshot(red, source_names, SOURCE_LINKS)
        changed = {name for name in source_names if red_files[name] != final_files[name]}
        require(changed == {product, 'EXTRACTION.json'}, 'Red has an unintended source difference: ' + label)
        require(all(red_files[name] == checksum for name, checksum in final_files.items()
                    if name.startswith('tests/acceptance/')), 'Red regression changed: ' + label)
        controls[label] = red
        records[label] = {'files': red_files, 'counterfactual': True, 'dirty': True,
                          'changed_files': sorted(changed), **control}
    save(args.output / 'evidence/source-identity.json', {
        'final_revision': FINAL, 'final_tree': git(args.source, 'rev-parse', 'HEAD^{tree}'),
        'engine_revision': ENGINE, 'final_files': final_files, 'controls': records,
    })
    return controls, final_files


def sdk_identity(args):
    # Existing production package verifier checks identity, all files and symlinks.
    from package_sdk import previous_package
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
    save(args.output / 'evidence/sdk-identity.json', result)
    return result


def build_command(args, name, command):
    path = args.output / 'logs' / (name + '.log')
    with path.open('w') as log:
        print('+ ' + shlex.join(command), flush=True)
        code = subprocess.run(command, cwd=args.engine, stdout=log, stderr=subprocess.STDOUT).returncode
    if code:
        print(path.read_text()[-8000:], end='', flush=True)
    require(code == 0, f'{name} failed with exit {code}; see its build log')
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
                                    'bin/studio-acceptance'], text=True)
    require('-Wl,--strip-debug' in link.splitlines()[-1], 'Missing link-time debug stripping')
    (args.output / 'evidence' / (name + '-build-commands.txt')).write_text(link)
    shutil.copy2(build / 'compile_commands.json', args.output / 'evidence' / (name + '-compile_commands.json'))
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
    return build, evidence_path, native, evidence


def fixture_directories(source):
    prefixes = set()
    for path in (source / 'tests/acceptance').rglob('*'):
        if path.is_file() and path.suffix in ('.c', '.h'):
            prefixes.update(re.findall(r'"/tmp/([^"/]*?)XXXXXX"', path.read_text()))
    return {str(path) for prefix in prefixes for path in Path('/tmp').glob(prefix + '*')}


def supervised(args, name, commands, env):
    from test_policy import run_commands
    path = args.output / 'logs' / (name + '.log')
    fixtures_before = fixture_directories(args.source)
    with path.open('w') as log:
        code = run_commands(commands, seconds=60, env=env, output=log)
    output = path.read_text()
    print(output, end='', flush=True)
    new_fixtures = sorted(fixture_directories(args.source) - fixtures_before)
    save(args.output / 'evidence' / (name + '-result.json'), {
        'exit_code': code, 'log_sha256': digest(path), 'new_hardcoded_tmp_fixtures': new_fixtures})
    require(not new_fixtures, 'Test left scoped /tmp fixtures (retained, not deleted): ' + ', '.join(new_fixtures))
    return code, output


def graphics_environment(args):
    env = os.environ.copy()
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'SDL_VIDEODRIVER'):
        env.pop(key, None)
    paths = subprocess.check_output(['dpkg-query', '-L', 'mesa-vulkan-drivers'], text=True).splitlines()
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
    save(args.output / 'evidence/backend.json', {'icd': str(icds[0]), 'icd_sha256': digest(icds[0]),
         'mesa_package': subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', 'mesa-vulkan-drivers'], text=True),
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
    if name in CONTROLS:
        control = CONTROLS[name]
        reason = report.get('reason', '')
        require(code != 0 and report.get('status') == 'failed' and reason.startswith('child_exit_1:'),
                'Red did not fail as the intended assertion workload')
        require(all(marker in reason for marker in control['markers']) and
                re.search(control['assertion'], reason),
                'Red did not reproduce its exact reviewed defect assertion: ' + name)
        require(SUCCESS not in reason, 'Red unexpectedly reported success')
    else:
        require(code == 0 and report.get('status') == 'passed' and
                report.get('reason') == 'admitted_studio_acceptance_completed' and
                report.get('execution_evidence') == {'frames': 4, 'edits': 1, 'undo': 1, 'redo': 1},
                'Green source-edit did not pass all existing/new assertions')
    require(not any((args.output / 'scratch').iterdir()), 'Supervised test left temporary fixture state')


def cpu_inventory(selector, code, output, cases, assertions):
    require(code == 0 and not any(marker in output for marker in PROHIBITED), selector + ' failed')
    plain = re.sub(r'\x1b\[[0-9;]*m', '', output)
    summaries = re.findall(r'Summary: Passed: (\d+) Failed: (\d+) Total: (\d+) Suites: (\d+) Asserts: (\d+)', plain)
    require(len(summaries) == 1, selector + ' has no unique pico summary')
    passed, failed, total, suites, observed_assertions = map(int, summaries[0])
    require((passed, failed, total, observed_assertions) == (cases, 0, cases, assertions),
            selector + ' case/assertion inventory differs from reviewed admission')
    return {'passed': passed, 'failed': failed, 'total': total, 'suites': suites, 'assertions': observed_assertions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'baseline', 'engine', 'sdk', 'validation', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(exist_ok=False)
    for directory in ('logs', 'evidence'):
        (args.output / directory).mkdir()
    os.chdir(args.engine)
    sys.path.insert(0, str(args.engine / 'tools'))
    policy_hashes = snapshot(args.engine, POLICY)
    controls, final_files = source_inputs(args)
    sdk = sdk_identity(args)
    env = graphics_environment(args)
    control_builds = []
    for label, source in controls.items():
        artifact = build_variant(args, label, source, sdk)
        control_builds.append(artifact[0])
        gpu_test(args, label, source, artifact, env)
    green_artifact = build_variant(args, 'green', args.source, sdk)
    gpu_test(args, 'green', args.source, green_artifact, env)
    binary = green_artifact[0] / 'bin/studio-acceptance'
    policy_source = args.source / 'projects/studio/content/libraries/OutlinerRowPolicies.chibi'
    for selector, extra, cases, assertions in (
            ('--project-open', [], 1, 50), ('--scene-read', [], 6, 1141),
            ('--outliner-row-policies', ['--source', str(policy_source)], 1, 1848)):
        require(digest(binary) == green_artifact[3]['binary_sha256'], 'Green binary changed before CPU selector')
        code, output = supervised(args, selector[2:], [[str(binary), selector, *extra]], env)
        save(args.output / 'evidence' / (selector[2:] + '-inventory.json'),
             cpu_inventory(selector, code, output, cases, assertions))
    require(snapshot(args.source, final_files, SOURCE_LINKS) == final_files,
            'Final source changed during validation')
    clean_checkout(args.source, FINAL)
    clean_checkout(args.engine, ENGINE)
    require(snapshot(args.engine, POLICY) == policy_hashes, 'Production admission/supervisor changed')
    require(sdk_identity(args) == sdk, 'Installed SDK changed during validation')
    require(not any((args.output / 'scratch').iterdir()), 'Temporary fixture cleanup incomplete')
    save(args.output / 'summary.json', {'passed': True, 'final_revision': FINAL, 'engine_revision': ENGINE,
         'red_controls': {label: 'exact reviewed assertion reproduced' for label in controls},
         'green': 'four-frame source-edit accepted',
         'cpu': ['project-open', 'scene-read', 'outliner-row-policies'], 'full_suite': 'not run',
         'rendering': 'Xvfb/X11 with forced Vulkan and verified Mesa software ICD',
         'policy_sha256': policy_hashes, 'sdk_build_id': sdk['build_id'],
         'logging': 'agent failure tail and success JSON; complete raw child logs are not exported'})
    for path in (*controls.values(), *control_builds, green_artifact[0], args.output / 'scratch',
                 args.output / 'cache', args.output / 'prefs'):
        shutil.rmtree(path)
    print('Reviewed Save/Discard defects reproduced; clean final four-frame and three CPU checks passed.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Validation blocked/failed: ' + str(error), file=sys.stderr, flush=True)
        sys.exit(1)
