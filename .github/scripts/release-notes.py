"""Generate categorized release notes from Conventional Commit subjects.

用法：python release-notes.py --tag v0.1.0-beta.2 --sha <commit> --repo DiceZone/CatOfficial --output notes.md
从上一个 v* 标签到 --sha 之间的提交按 feat / fix / perf 等前缀分组；没有上一个标签时列出全部提交。
"""
import argparse
import re
import subprocess

GROUPS = [
    ('feat', '新功能'),
    ('fix', '修复'),
    ('perf', '性能'),
    ('refactor', '重构'),
    ('docs', '文档'),
]
OTHER = '其他'
SKIP = {'chore', 'ci', 'test', 'style', 'build'}
SUBJECT = re.compile(r'^(?P<type>[a-z]+)(?:\((?P<scope>[^)]*)\))?(?P<bang>!)?:\s*(?P<text>.+)$')


def git(*args: str) -> str:
    return subprocess.run(['git', *args], check=True, capture_output=True, text=True, encoding='utf-8').stdout


def previous_tag(sha: str, tag: str) -> str | None:
    tags = [t for t in git('tag', '--merged', sha, '--sort=-creatordate', '--list', 'v*').split() if t != tag]
    return tags[0] if tags else None


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', required=True)
    parser.add_argument('--sha', required=True)
    parser.add_argument('--repo', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()

    base = previous_tag(args.sha, args.tag)
    span = f'{base}..{args.sha}' if base else args.sha
    log = git('log', '--no-merges', '--format=%h%x09%s', span)
    groups: dict[str, list[str]] = {}
    for line in log.splitlines():
        short, _, subject = line.partition('\t')
        match = SUBJECT.match(subject)
        kind = match.group('type') if match else ''
        if kind in SKIP:
            continue
        title = dict(GROUPS).get(kind, OTHER)
        text = match.group('text') if match else subject
        scope = match.group('scope') if match else None
        entry = f'- {f"**{scope}**：" if scope else ""}{text}（{short}）'
        groups.setdefault(title, []).append(entry)

    lines = [f'## CatOfficial {args.tag.lstrip("v")}', '']
    order = [title for _, title in GROUPS] + [OTHER]
    for title in order:
        if title in groups:
            lines += [f'### {title}', '', *groups[title], '']
    if not groups:
        lines += ['本次没有面向用户的改动。', '']
    lines += [
        '## 下载',
        '',
        '| 系统 | 文件 |',
        '| --- | --- |',
        '| Windows 10/11（x64） | `CatOfficial-*-windows-amd64.zip` |',
        '| Windows 11（ARM64） | `CatOfficial-*-windows-arm64.zip` |',
        '| Linux（x86_64） | `CatOfficial-*-linux-amd64.tar.gz` |',
        '| Linux（ARM64） | `CatOfficial-*-linux-arm64.tar.gz` |',
        '| macOS（Apple 芯片） | `CatOfficial-*-macos-arm64.tar.gz` |',
        '',
        '解压出 `CatOfficial-beta` 文件夹后运行其中的 `catofficial`（Windows 双击 `catofficial.exe`），浏览器打开 http://localhost:18082 进入管理面板。已安装的可以在面板「关于」里在线更新；手动升级时覆盖解压即可，`config.json` 与 `data` 不受影响。',
        '各文件的 SHA-256 见 `SHA256SUMS.txt`。',
        '',
    ]
    if base:
        lines.append(f'完整变更：https://github.com/{args.repo}/compare/{base}...{args.tag}')
    with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines).rstrip() + '\n')


if __name__ == '__main__':
    main()
