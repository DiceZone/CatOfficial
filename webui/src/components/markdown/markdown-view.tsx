// Markdown 显示：Dice!Next 回复预览（markdown-example）的同一套子集，另外认图片 ![说明](地址)。
// 官方素材引用 qqbot-media:// 浏览器打不开，显示成一个占位标签。

import React from 'react';
import { ImageIcon } from 'lucide-react';

const unescapeMarkdown = (text: string): string => text.replace(/\\([!-/:-@[-`{-~])/g, '$1');
const safeUrl = (url: string) => /^(?:https?:\/\/|\/api\/webui\/)/i.test(url.trim());

const inlineMarkdown = (text: string, prefix: string, onImage?: (src: string) => void): React.ReactNode[] => {
  const pattern = /(\\[!-/:-@[-`{-~]|!\[[^\]\n]*\]\([^)\n]+\)|\*\*[^*\n]+\*\*|__[^_\n]+__|~~[^~\n]+~~|\*[^*\n]+\*|_[^_\n]+_|`[^`\n]+`|\[[^\]\n]+\]\([^)\n]+\)|<qqbot-at-everyone\s*\/>|<@!?[^>\s]+>)/g;
  const out: React.ReactNode[] = [];
  let last = 0, index = 0;
  for (const match of text.matchAll(pattern)) {
    const start = match.index ?? 0;
    if (start > last) out.push(text.slice(last, start));
    const token = match[0];
    const key = `${prefix}-${index++}`;
    if (token.startsWith('\\')) out.push(token.slice(1));
    else if (token.startsWith('![')) {
      const parsed = token.match(/^!\[([^\]]*)\]\(([^)\s]+)(?:\s[^)]*)?\)$/);
      const alt = (parsed?.[1] ?? '').replace(/\s*#\d+px\s*#\d+px\s*$/, '');
      const url = parsed?.[2] ?? '';
      if (url && safeUrl(url))
        out.push(<button key={key} type="button" onClick={() => onImage?.(url)} className="my-1 block overflow-hidden rounded-md border bg-muted">
          <img src={url} alt={alt} loading="lazy" referrerPolicy="no-referrer" className="max-h-60 max-w-full object-contain" /></button>);
      else out.push(<span key={key} className="inline-flex items-center gap-1 rounded border bg-muted px-1.5 py-0.5 text-xs text-muted-foreground"><ImageIcon className="h-3 w-3" />{alt || url}</span>);
    }
    else if ((token.startsWith('**') && token.endsWith('**')) || (token.startsWith('__') && token.endsWith('__')))
      out.push(<strong key={key}>{unescapeMarkdown(token.slice(2, -2))}</strong>);
    else if ((token.startsWith('*') && token.endsWith('*')) || (token.startsWith('_') && token.endsWith('_')))
      out.push(<em key={key}>{unescapeMarkdown(token.slice(1, -1))}</em>);
    else if (token.startsWith('~~')) out.push(<del key={key}>{unescapeMarkdown(token.slice(2, -2))}</del>);
    else if (token.startsWith('`')) out.push(<code key={key} className="rounded bg-muted px-1 py-0.5 font-mono text-[0.9em]">{token.slice(1, -1)}</code>);
    else if (token.startsWith('<qqbot-at-everyone')) out.push(<span key={key} className="text-primary">@全体成员</span>);
    else if (token.startsWith('<@')) out.push(<span key={key} className="text-primary">@{token.replace(/^<@!?/, '').replace(/>$/, '').slice(0, 8)}…</span>);
    else {
      const parsed = token.match(/^\[([^\]]+)\]\(([^)]+)\)$/);
      const safe = parsed && /^(?:https?:\/\/|#)/i.test(parsed[2].trim());
      out.push(parsed && safe ? <a key={key} href={parsed[2]} className="text-primary underline" target="_blank" rel="noreferrer">{unescapeMarkdown(parsed[1])}</a> : token);
    }
    last = start + token.length;
  }
  if (last < text.length) out.push(text.slice(last));
  return out;
};

function tableCells(line: string) {
  const trimmed = line.trim();
  if (!trimmed.startsWith('|') || !trimmed.endsWith('|')) return [];
  const cells: string[] = [];
  let cell = '';
  for (let i = 1; i < trimmed.length - 1; i++) {
    const ch = trimmed[i];
    if (ch === '\\' && i + 1 < trimmed.length - 1) cell += ch + trimmed[++i];
    else if (ch === '|') { cells.push(cell.trim()); cell = ''; }
    else cell += ch;
  }
  cells.push(cell.trim());
  return cells;
}

export function MarkdownView({ text, onImage }: { text: string; onImage?: (src: string) => void }) {
  const lines = text.split(/\r?\n/);
  let nextLine = 0;
  return <div className="space-y-1 break-words [overflow-wrap:anywhere]">{lines.map((line, i) => {
    if (i < nextLine) return null;
    const header = tableCells(line), separator = tableCells(lines[i + 1] ?? '');
    if (header.length && header.length === separator.length && separator.every((cell) => /^:?-{3,}:?$/.test(cell))) {
      const rows: string[][] = [];
      nextLine = i + 2;
      while (nextLine < lines.length) {
        const cells = tableCells(lines[nextLine]);
        if (!cells.length) break;
        rows.push(cells);
        nextLine++;
      }
      return <div key={i} className="max-w-full overflow-x-auto"><table className="border-collapse text-left text-sm">
        <thead><tr>{header.map((cell, c) => <th key={c} className="border px-2 py-1">{inlineMarkdown(cell, 'h' + c, onImage)}</th>)}</tr></thead>
        <tbody>{rows.map((row, r) => <tr key={r}>{header.map((_, c) => <td key={c} className="border px-2 py-1">{inlineMarkdown(row[c] ?? '', r + '-' + c, onImage)}</td>)}</tr>)}</tbody>
      </table></div>;
    }
    const heading = line.match(/^(#{1,6})\s+(.*)$/);
    if (heading) return <div key={i} className="font-bold" style={{ fontSize: `${Math.max(1, 1.45 - heading[1].length * 0.08)}rem` }}>{inlineMarkdown(heading[2], `h${i}`, onImage)}</div>;
    if (/^>\s?/.test(line)) return <blockquote key={i} className="border-l-2 border-primary/40 pl-3 text-muted-foreground">{inlineMarkdown(line.replace(/^>\s?/, ''), `q${i}`, onImage)}</blockquote>;
    if (/^\s*(?:\*\s*){3,}$|^\s*(?:-\s*){3,}$|^\s*(?:_\s*){3,}$/.test(line)) return <hr key={i} className="my-2 border-border" />;
    if (/^[-+*]\s+/.test(line)) return <div key={i} className="flex gap-2"><span>•</span><span>{inlineMarkdown(line.replace(/^[-+*]\s+/, ''), `l${i}`, onImage)}</span></div>;
    const ordered = line.match(/^(\d+)[.)]\s+(.*)$/);
    if (ordered) return <div key={i} className="flex gap-2"><span className="tabular-nums">{ordered[1]}.</span><span>{inlineMarkdown(ordered[2], `o${i}`, onImage)}</span></div>;
    if (!line) return <div key={i} className="h-2" />;
    return <div key={i}>{inlineMarkdown(line, `p${i}`, onImage)}</div>;
  })}</div>;
}
