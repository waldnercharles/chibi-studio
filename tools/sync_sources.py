#!/usr/bin/env python3
"""Refresh only the original source paths recorded in Studio's extraction manifest."""

import argparse
import hashlib
import json
from pathlib import Path
import re


def checksum(data):
    return hashlib.sha256(data).hexdigest()


def sync(source, studio):
    manifest_path = studio / 'EXTRACTION.json'
    manifest = json.loads(manifest_path.read_text())
    owned = set(manifest['consumer_owned_files'])
    pending = []
    for name, record in manifest['files'].items():
        origin = source / record['source_path']
        destination = studio / name
        if (not origin.resolve().is_relative_to(source)
                or not destination.resolve().is_relative_to(studio)):
            raise ValueError(f'Extraction path escapes its repository: {name}')
        raw = origin.read_bytes()
        data = raw
        for before, after in record.get('consumer_replacements', []):
            data = data.replace(before.encode(), after.encode())
        pending.append((name, record, raw, data))
    # Resolve every recorded source before changing the consumer snapshot.
    for name, record, raw, data in pending:
        destination = studio / name
        record['current_source_sha256'] = checksum(raw)
        if name not in owned:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    manifest['missing_current_sources'] = []
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    headers = {}
    for path in sorted((studio / 'src').rglob('*')):
        if path.suffix not in ('.c', '.h', '.cpp'):
            continue
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', path.read_text(), re.M):
            if any((base / name).is_file() for base in (path.parent, studio / 'src', studio / 'include')):
                continue
            headers.setdefault(name, []).append(path.relative_to(studio).as_posix())
    (studio / 'SDK_HEADERS.json').write_text(json.dumps(
        {name: sorted(set(paths)) for name, paths in sorted(headers.items())}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    try:
        sync(args.source.resolve(), Path(__file__).resolve().parents[1])
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'sync-sources: {error}\n')


if __name__ == '__main__':
    main()
