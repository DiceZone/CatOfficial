"""Write update-manifest.json for CatOfficial's online updater.

用法：python update-manifest.py --dir artifacts --tag v0.1.0-beta.2 --version 0.1.0 --build 2 \
        --repo DiceZone/CatOfficial --notes release-notes.md --output artifacts/update-manifest.json
程序里的检查逻辑（app/update/update_service.cpp 的 parseReleaseManifest）要求：
schema 1、repository 与程序内置的一致、tag 是 v<版本> 或 v<版本>-beta.<构建号>、每个平台一个安装包。
"""
import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

TARGETS = [
    ('windows', 'amd64', 'zip'),
    ('windows', 'arm64', 'zip'),
    ('linux', 'amd64', 'tar.gz'),
    ('linux', 'arm64', 'tar.gz'),
    ('macos', 'arm64', 'tar.gz'),
]
SAFE_NAME = re.compile(r'^[A-Za-z0-9._()\-]+$')


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--dir', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--build', type=int, required=True)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--notes', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()

    folder = Path(args.dir)
    assets = []
    for os_name, arch, ext in TARGETS:
        matches = sorted(folder.glob(f'CatOfficial-*-{os_name}-{arch}.{ext}'))
        if len(matches) != 1:
            sys.exit(f'Expected exactly one asset for {os_name}-{arch}, found {len(matches)}')
        path = matches[0]
        if not SAFE_NAME.match(path.name):
            sys.exit(f'Release asset name contains characters GitHub may rewrite: {path.name}')
        assets.append({'os': os_name, 'arch': arch, 'name': path.name, 'sha256': sha256(path), 'size': path.stat().st_size})

    notes = Path(args.notes).read_text(encoding='utf-8')
    # 只放面向用户的改动：下载表与比较链接不需要。
    notes = notes.split('## 下载')[0].strip()[:16000]
    manifest = {
        'schema': 1,
        'repository': args.repo,
        'tag': args.tag,
        'version': args.version,
        'build': args.build,
        'prerelease': args.build > 0,
        'published_at': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'release_url': f'https://github.com/{args.repo}/releases/tag/{args.tag}',
        'release_notes': notes,
        'assets': assets,
    }
    Path(args.output).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in manifest.items() if k != 'release_notes'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
