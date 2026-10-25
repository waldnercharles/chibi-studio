"""Small offline API/package inputs for the pico_unit Pages dispatcher."""

import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import zipfile

sys.path.insert(0, sys.argv[1])
import pages_site as pages

kind, output = sys.argv[2], Path(sys.argv[3])


def restore():
    def bundle(label):
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode='w') as archive:
            for name, data in {'index.html': (label + ' production\n').encode(),
                               'previews/78/keep.txt': (label + ' preview78\n').encode()}.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        zipped = io.BytesIO()
        with zipfile.ZipFile(zipped, 'w') as archive:
            archive.writestr('artifact.tar', raw.getvalue())
        return zipped.getvalue()
    data, older_data = bundle('current'), bundle('older')
    job_url = f'https://github.com/{pages.REPOSITORY}/actions/runs/20/job/200'
    deployments = [dict(id=3, environment='github-pages', ref='main', sha=pages.STUDIO),
                   dict(id=2, environment='github-pages', ref='main', sha=pages.STUDIO),
                   dict(id=1, environment='github-pages', ref='main', sha=pages.STUDIO)]
    status = dict(id=200, state='success', created_at='2026-10-25T10:00:00Z', log_url=job_url)
    run = dict(id=20, head_branch='main', path=pages.WORKFLOW, head_sha=pages.STUDIO,
               head_repository=dict(full_name=pages.REPOSITORY), event='push', run_attempt=1)
    job = dict(id=200, run_id=20, name='deploy', run_attempt=1,
               steps=[dict(name='Deploy the reusable editor', conclusion='success')])
    payload = dict(artifact_id=2000, pages_build_version=pages.STUDIO, oidc_token='***')
    artifact = dict(id=2000, name='github-pages', expired=False, size_in_bytes=len(data),
                    digest='sha256:' + hashlib.sha256(data).hexdigest(),
                    workflow_run=dict(id=20, head_sha=pages.STUDIO, head_branch='main'))
    if kind == 'failed-rerun':
        run['run_attempt'] = 2  # Attempt 2 failed; its newer upload is not the live site.
    if kind == 'untrusted-workflow':
        run['path'] = '.github/workflows/untrusted.yml'
    if kind == 'wrong-job-run':
        job['run_id'] = 21
    if kind == 'wrong-artifact-run':
        artifact['workflow_run']['id'] = 21
    if kind == 'expired-snapshot':
        artifact['expired'] = True
    statuses = [status]
    if kind == 'pagination':
        statuses = [dict(id=300 + i, state='inactive') for i in range(20)] + statuses
    older = dict(artifact, id=1000, created_at='2026-10-24T09:59:00Z',
                 workflow_run=dict(artifact['workflow_run'], id=10),
                 digest='sha256:' + hashlib.sha256(older_data).hexdigest())
    current = dict(artifact, created_at='2026-10-25T09:59:00Z')
    newer = dict(artifact, id=3000, created_at='2026-10-25T11:00:00Z',
                 workflow_run=dict(artifact['workflow_run'], id=30))
    routes = {
        'deployments?environment=github-pages&per_page=20&page=1': deployments,
        'deployments/3/statuses?per_page=20&page=1': [dict(state='failure')],
        'deployments/2/statuses?per_page=20&page=1': statuses[:20],
        'deployments/2/statuses?per_page=20&page=2': statuses[20:],
        'deployments/1/statuses?per_page=20&page=1': [
            dict(status, id=100, created_at='2026-10-24T10:00:00Z',
                 log_url=job_url.replace('/20/job/200', '/10/job/100'))],
        'actions/runs/20': run,
        'actions/jobs/200': job,
        'actions/jobs/200/logs': (
            '2026-10-25T10:00:00.000Z Creating Pages deployment with payload:\n' +
            '\n'.join('2026-10-25T10:00:00.000Z ' + line
                      for line in json.dumps(payload, indent=2).splitlines()) + '\n'
        ).encode(),
        'actions/artifacts/2000': artifact,
        'actions/artifacts/2000/zip': data,
        # These uploads exist but must never be considered. One is older than
        # the live deployment, the other belongs to a newer failed attempt.
        'actions/artifacts?name=github-pages&per_page=100': {
            'artifacts': [older, newer] if kind == 'missing-latest' else [older, current, newer]},
        'actions/runs/10': dict(run, id=10),
        'actions/runs/30': dict(run, id=30),
        'actions/runs/10/jobs?per_page=100': {'jobs': [dict(job, id=100, run_id=10)]},
        'actions/runs/30/jobs?per_page=100': {'jobs': [dict(job, id=300, run_id=30, steps=[])]},
        'actions/runs/20/jobs?per_page=100': {
            'jobs': [dict(job, run_attempt=2, steps=[]) if kind == 'failed-rerun' else job]},
        'actions/artifacts/1000/zip': older_data,
    }
    if kind == 'deployment-pagination':
        routes['deployments?environment=github-pages&per_page=20&page=1'] = [
            dict(deployments[0], id=100 + i) for i in range(20)]
        routes['deployments?environment=github-pages&per_page=20&page=2'] = deployments
        for i in range(20):
            routes[f'deployments/{100 + i}/statuses?per_page=20&page=1'] = [dict(state='failure')]
    if kind == 'missing-latest':
        routes.pop('actions/artifacts/2000')
    if kind == 'missing-log':
        routes.pop('actions/jobs/200/logs')
    if kind == 'ambiguous-payload':
        routes['actions/jobs/200/logs'] *= 2
    if kind == 'truncated-history':
        routes = {f'deployments?environment=github-pages&per_page=20&page={i}': (deployments * 7)[:20]
                  for i in range(1, 6)}
    calls = []

    def api(path, binary=False):
        calls.append(path)
        (output.parent / 'api-calls.json').write_text(json.dumps(calls))
        if path not in routes:
            raise ValueError('Mock API resource unavailable: ' + path)
        return routes[path]

    pages.api = api
    pages.restore(output)


def package():
    output.mkdir()
    for name in pages.FILES:
        (output / name).write_bytes(('incoming ' + name + '\n').encode())
    sdk = output.parent / 'sdk'
    manifest = sdk / 'share/chibi/package.json'
    manifest.parent.mkdir(parents=True)
    identity = dict(source=dict(revision=pages.ENGINE, dirty=kind == 'package-dirty-sdk'),
                    requirements=dict(system='Emscripten', web_threads=kind == 'package-threaded'),
                    build_id='a' * 64)
    if kind == 'package-wrong-engine':
        identity['source']['revision'] = '0' * 40
    manifest.write_text(json.dumps(identity))
    # Only git metadata is stubbed; package/archive/checksum code runs unchanged.
    def git(arguments, text=False):
        if arguments[-1] == 'HEAD':
            return pages.STUDIO + '\n'
        if arguments[-1] == 'HEAD^{tree}':
            return ('0' * 40 if kind == 'package-wrong-tree' else pages.TREE) + '\n'
        if arguments[-1] == '--porcelain':
            return ' M source.c\n' if kind == 'package-dirty-source' else ''
        raise ValueError('Unexpected package subprocess')
    pages.subprocess.check_output = git
    pages.package(output, output.parent / 'source', sdk, True)
    # Emit independent raw-file digests for C to compare with recorded metadata.
    record = json.loads((output / 'studio.json').read_text())
    (output.parent / 'recorded.json').write_text(json.dumps(record['files'], sort_keys=True))
    (output.parent / 'actual.json').write_text(json.dumps({
        name: hashlib.sha256((output / name).read_bytes()).hexdigest()
        for name in pages.FILES}, sort_keys=True))


def capture():
    binary = kind == 'api-binary'
    payload = b'\x1b[32mremote log\x1b[0m\n' if binary else b'{"id": 200}'

    def command(arguments, timeout):
        (output.parent / 'api-arguments.json').write_text(json.dumps(arguments))
        if binary and '--allow-escape-sequences' not in arguments:
            raise ValueError('Mock gh refuses terminal escape sequences')
        if not binary and '--allow-escape-sequences' in arguments:
            raise ValueError('JSON requests must keep default output protection')
        return payload

    pages.subprocess.check_output = command
    result = pages.api('actions/jobs/200/logs' if binary else 'actions/jobs/200', binary)
    output.write_bytes(result if binary else json.dumps(result).encode())


if kind.startswith('api-'):
    capture()
elif kind.startswith('package-'):
    package()
else:
    restore()
