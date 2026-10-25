#!/usr/bin/env python3
"""Compose the production Pages site and the explicitly approved PR77 preview."""

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

REPOSITORY = 'waldnercharles/chibi-studio'
WORKFLOW = '.github/workflows/publish.yml'
PREVIEW = 'previews/77'
STUDIO = 'c28301748699a158fec08a475ca7dde872bb4960'
TREE = '9cfd1b700234e09df5f5611792e94e36e08127da'
ENGINE = 'd1154cdb8677cbfe479d4be40db7ca480d10f103'
FILES = ('chibi-studio.html', 'chibi-studio.js', 'chibi-studio.wasm',
         'chibi-studio.data', 'chibi-default.js')
MAX_BYTES = 512 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sha(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{40}', value)


def inventory(root):
    require(root.is_dir() and not root.is_symlink(), 'Expected a regular site directory')
    files = {}
    total = 0
    for path in root.rglob('*'):
        require(not path.is_symlink(), 'Site symlinks are forbidden')
        require(path.is_file() or path.is_dir(), 'Site special files are forbidden')
        if path.is_file():
            total += path.stat().st_size
            require(total <= MAX_BYTES, 'Site exceeds the 512 MiB budget')
            files[path.relative_to(root).as_posix()] = digest(path)
    return files


def application(root, preview=False):
    files = inventory(root)
    record = json.loads((root / 'studio.json').read_text())
    require(record.get('format') == 1 and sha(record.get('studio')) and sha(record.get('engine')),
            'Invalid application identity')
    archive = f"editor-{record['studio']}-{record['engine']}.tar.gz"
    require(record.get('archive') == archive, 'Invalid application archive name')
    require(set(files) == set(FILES) | {'studio.json', 'index.html', '.nojekyll', archive},
            'Unexpected application files')
    require(record.get('sha256') == files[archive], 'Application archive checksum mismatch')
    if preview:
        require((record['studio'], record.get('tree'), record['engine']) == (STUDIO, TREE, ENGINE),
                'Preview77 source/tree/engine must match the reviewed checkpoint')
        require(record.get('web_threads') is False, 'Preview77 must be serial')
        require(re.fullmatch('[0-9a-f]{64}', record.get('sdk_build_id', '')),
                'Preview77 SDK identity is missing')
        require(record.get('files') == {name: files[name] for name in FILES},
                'Preview77 application file checksum mismatch')
    return record


def package(site, source, sdk, preview):
    require(set(inventory(site)) == set(FILES), 'Expected only the five built serial files')
    studio = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    tree = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD^{tree}'], text=True).strip()
    require(not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True),
            'Application source must be clean')
    manifest = sdk / 'share/chibi/package.json'
    record = json.loads(manifest.read_text())
    engine = record['source']['revision']
    require(not record['source']['dirty'] and sha(engine), 'SDK source must be exact and clean')
    require(record['requirements']['system'] == 'Emscripten' and
            record['requirements']['web_threads'] is False, 'Expected the serial web SDK')
    if preview:
        require((studio, tree, engine) == (STUDIO, TREE, ENGINE), 'Unapproved preview input')
    archive = f'editor-{studio}-{engine}.tar.gz'
    with tarfile.open(site / archive, 'w:gz') as output:
        for name in FILES:
            output.add(site / name, arcname=name, recursive=False)
    identity = dict(format=1, studio=studio, tree=tree, engine=engine,
                    archive=archive, sha256=digest(site / archive), web_threads=False,
                    sdk_build_id=record['build_id'], sdk_manifest_sha256=digest(manifest),
                    files={name: digest(site / name) for name in FILES})
    (site / 'studio.json').write_text(json.dumps(identity, indent=2) + '\n')
    (site / 'index.html').write_text('<!doctype html><meta charset="utf-8">'
                                   '<meta http-equiv="refresh" content="0;url=chibi-studio.html">'
                                   '<a href="chibi-studio.html">Open Chibi Studio</a>\n')
    (site / '.nojekyll').touch()
    application(site, preview)


def compose(mode, previous, built, output, pr, main):
    require(mode in ('production', 'preview77', 'cleanup77'), 'Unknown publication mode')
    require(not output.exists() and output.parent.is_dir(), 'Output must be a new directory')
    require(not output.resolve().is_relative_to(previous.resolve()), 'Output overlaps previous site')
    if built is not None:
        require(not output.resolve().is_relative_to(built.resolve()), 'Output overlaps application')
    before = inventory(previous)
    require('studio.json' in before, 'Previous production site identity is missing')
    root = json.loads((previous / 'studio.json').read_text())
    require(root.get('format') == 1 and sha(root.get('studio')) and sha(root.get('engine')),
            'Previous production identity is invalid')
    require(pr.get('number') == 77 and isinstance(pr.get('merged'), bool), 'Invalid PR77 state')
    if mode == 'cleanup77':
        require(pr['merged'], 'Cleanup requires a merged PR77')
    else:
        require(built is not None, 'Application input is required')
        incoming = application(built, mode == 'preview77')
        if mode == 'preview77':
            require(pr.get('state') == 'open' and not pr['merged'],
                    'A closed or merged PR77 cannot publish a preview')
        else:
            require(sha(main) and incoming['studio'] == main, 'Production build is stale')
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary) / 'site'
        shutil.copytree(previous, stage)
        if mode == 'production':
            for path in stage.iterdir():
                if path.name != 'previews':
                    shutil.rmtree(path) if path.is_dir() else path.unlink()
            for path in built.iterdir():
                shutil.copy2(path, stage / path.name)
        else:
            target = stage / PREVIEW
            if target.exists():
                require(target.is_dir(), 'Preview77 destination is not a directory')
                shutil.rmtree(target)
            if mode == 'preview77':
                target.parent.mkdir(exist_ok=True)
                shutil.copytree(built, target)
        after = inventory(stage)
        # Verify the untouched part of the complete site, including other previews.
        retained = (lambda name: name.startswith('previews/')) if mode == 'production' else (
            lambda name: not name.startswith(PREVIEW + '/'))
        require({k: v for k, v in before.items() if retained(k)} ==
                {k: v for k, v in after.items() if retained(k)}, 'Publication changed retained files')
        stage.rename(output)


def api(path, binary=False):
    result = subprocess.check_output(['gh', 'api', f'repos/{REPOSITORY}/{path}'], timeout=60)
    return result if binary else json.loads(result)


def unpack(data, expected, output):
    require(len(data) <= MAX_BYTES and hashlib.sha256(data).hexdigest() == expected,
            'Pages artifact checksum/budget mismatch')
    require(not output.exists(), 'Restore destination already exists')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        require(archive.namelist() == ['artifact.tar'], 'Expected the Pages artifact.tar wrapper')
        require(archive.getinfo('artifact.tar').file_size <= MAX_BYTES, 'Pages tar exceeds budget')
        with tarfile.open(fileobj=io.BytesIO(archive.read('artifact.tar'))) as site:
            members = site.getmembers()
            require(len(members) <= 4096 and all(m.size >= 0 for m in members) and sum(m.size for m in members) <= MAX_BYTES,
                    'Pages site exceeds extraction budget')
            names = set()
            for member in members:
                name = member.name.removeprefix('./').rstrip('/')
                if name in ('', '.') and member.isdir():
                    continue
                path = PurePosixPath(name)
                require(name and not path.is_absolute() and '..' not in path.parts and
                        path.as_posix() == name and '\\' not in name and not any(ord(c) < 32 for c in name),
                        'Invalid Pages member path')
                require(member.isfile() or member.isdir(), 'Pages links/special files are forbidden')
                require(name not in names, 'Duplicate Pages member')
                names.add(name)
            output.mkdir()
            for member in members:
                name = member.name.removeprefix('./').rstrip('/')
                if name in ('', '.'):
                    continue
                target = output / name
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with site.extractfile(member) as source, target.open('wb') as destination:
                        shutil.copyfileobj(source, destination)
            inventory(output)


def restore(output):
    # Deployment history survives artifact deletion. Establish the live site's
    # identity first; never search surviving uploads for a plausible predecessor.
    requests = 0

    def read(path, binary=False):
        nonlocal requests
        requests += 1
        require(requests <= 100, 'Pages provenance exceeds the 100-request budget')
        return api(path, binary)

    def pages(path):
        for page in range(1, 6):
            separator = '&' if '?' in path else '?'
            records = read(f'{path}{separator}per_page=20&page={page}')
            require(isinstance(records, list) and len(records) <= 20,
                    'Invalid deployment history response')
            yield from records
            if len(records) < 20:
                return
        raise ValueError('Pages provenance history truncated; refusing rollback')

    successful = []
    for deployment in list(pages('deployments?environment=github-pages')):
        deployment_id = deployment['id']
        require(type(deployment_id) is int and deployment_id > 0,
                'Invalid deployment identity')
        for status in pages(f'deployments/{deployment_id}/statuses'):
            if status['state'] == 'success':
                require(re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', status['created_at'])
                        and type(status['id']) is int, 'Invalid deployment success identity')
                successful.append((status['created_at'], status['id'], deployment, status))
    require(successful, 'No successful Pages deployment found; refusing an empty-site deployment')
    _, _, deployment, status = max(successful, key=lambda item: item[:2])
    require(deployment['environment'] == 'github-pages' and deployment['ref'] == 'main',
            'Latest successful Pages deployment is not trusted main')
    # GitHub's environment status identifies the exact job, even when a later
    # workflow attempt failed. Do not use the jobs API's default filter=latest.
    job_url = re.fullmatch(re.escape(f'https://github.com/{REPOSITORY}/actions/runs/') +
                           r'([1-9][0-9]*)/job/([1-9][0-9]*)', status['log_url'])
    require(job_url, 'Latest successful deployment has no exact trusted job URL')
    run_id, job_id = map(int, job_url.groups())
    run = read(f'actions/runs/{run_id}')
    require(run['id'] == run_id and run['head_branch'] == 'main' and run['path'] == WORKFLOW and
            run['head_repository']['full_name'] == REPOSITORY and run['head_sha'] == deployment['sha'] and
            run['event'] in ('push', 'schedule', 'workflow_dispatch', 'pull_request_target'),
            'Latest successful deployment workflow is not trusted')
    job = read(f'actions/jobs/{job_id}')
    require(job['id'] == job_id and job['run_id'] == run_id and job['name'] == 'deploy' and
            type(job['run_attempt']) is int and 0 < job['run_attempt'] <= run['run_attempt'] and
            any(step['name'] == 'Deploy the reusable editor' and step['conclusion'] == 'success'
                for step in job.get('steps', [])), 'Exact Pages deployment job did not succeed')
    # deploy-pages v4 records the immutable artifact ID in its request payload.
    # A missing log or deleted artifact is a stop, including a same-run reupload.
    logs = read(f'actions/jobs/{job_id}/logs', binary=True)
    require(len(logs) <= 4 * 1024 * 1024, 'Pages job log exceeds the 4 MiB budget')
    logs = re.sub(r'^\ufeff?\d{4}-\d{2}-\d{2}T[0-9:.]+Z ', '', logs.decode('utf-8-sig'), flags=re.M)
    payloads = re.findall(r'^Creating Pages deployment with payload:\n(\{.*?^\})$', logs, re.M | re.S)
    require(len(payloads) == 1, 'Exact deployed artifact payload is missing or ambiguous')
    payload = json.loads(payloads[0])
    artifact_id = payload['artifact_id']
    require(type(artifact_id) is int and artifact_id > 0 and
            payload['pages_build_version'] == run['head_sha'], 'Invalid deployed artifact identity')
    artifact = read(f'actions/artifacts/{artifact_id}')
    require(artifact['id'] == artifact_id and artifact['name'] == 'github-pages' and
            artifact['workflow_run']['id'] == run_id and
            artifact['workflow_run']['head_sha'] == run['head_sha'] and
            artifact['workflow_run']['head_branch'] == 'main', 'Deployed artifact provenance mismatch')
    require(artifact['expired'] is False, 'Last deployed Pages snapshot expired; refusing rollback')
    require(0 < artifact['size_in_bytes'] <= MAX_BYTES, 'Pages artifact exceeds download budget')
    checksum = artifact.get('digest', '')
    require(re.fullmatch('sha256:[0-9a-f]{64}', checksum), 'Pages artifact digest is missing')
    unpack(read(f'actions/artifacts/{artifact_id}/zip', binary=True), checksum[7:], output)
    print(f"Restored deployment {deployment['id']}, run {run_id}, attempt {job['run_attempt']}, "
          f"artifact {artifact_id}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    stamp = commands.add_parser('package')
    for name in ('site', 'source', 'sdk'):
        stamp.add_argument('--' + name, type=Path, required=True)
    stamp.add_argument('--preview77', action='store_true')
    load = commands.add_parser('restore')
    load.add_argument('--output', type=Path, required=True)
    merge = commands.add_parser('compose')
    merge.add_argument('--mode', choices=('production', 'preview77', 'cleanup77'), required=True)
    for name in ('previous', 'built', 'output', 'pr'):
        merge.add_argument('--' + name, type=Path, required=name != 'built')
    merge.add_argument('--main', default='')
    args = parser.parse_args()
    try:
        if args.command == 'package':
            package(args.site, args.source, args.sdk, args.preview77)
        elif args.command == 'restore':
            restore(args.output)
        else:
            compose(args.mode, args.previous, args.built, args.output,
                    json.loads(args.pr.read_text()), args.main)
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired, tarfile.TarError, zipfile.BadZipFile) as error:
        parser.exit(1, f'pages-site: {error}\n')


if __name__ == '__main__':
    main()
