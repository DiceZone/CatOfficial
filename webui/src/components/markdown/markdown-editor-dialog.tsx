// 发送 Markdown 消息的弹窗：左边编辑（Dice!Next 的格式工具栏与快捷键），右边实时预览。

import React from 'react';
import { useTranslation } from 'react-i18next';
import { Send } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Textarea } from '@/components/ui/textarea';
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { MarkdownToolbar, useMarkdownEditing } from './markdown-toolbar';
import { MarkdownView } from './markdown-view';

interface Props {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  description?: string;
  initial?: string;
  submitLabel: string;
  busy?: boolean;
  extra?: React.ReactNode;          // 工具栏右侧的额外按钮（例如 @全体成员）
  onSubmit: (markdown: string) => void | Promise<void>;
}

export function MarkdownEditorDialog({ open, onOpenChange, title, description, initial = '', submitLabel, busy, extra, onSubmit }: Props) {
  const { t } = useTranslation();
  const [text, setText] = React.useState(initial);
  const ref = React.useRef<HTMLTextAreaElement>(null);
  const keys = useMarkdownEditing({ textareaRef: ref, value: text, onChange: setText });
  React.useEffect(() => { if (open) setText(initial); }, [open, initial]);
  const insert = (snippet: string) => {
    const area = ref.current;
    const start = area?.selectionStart ?? text.length, end = area?.selectionEnd ?? text.length;
    setText(text.slice(0, start) + snippet + text.slice(end));
    requestAnimationFrame(() => { area?.focus(); area?.setSelectionRange(start + snippet.length, start + snippet.length); });
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-5xl">
        <DialogHeader>
          <DialogTitle>{title}</DialogTitle>
          {description && <DialogDescription>{description}</DialogDescription>}
        </DialogHeader>
        <div className="grid min-w-0 gap-4 lg:grid-cols-2">
          <div className="flex min-w-0 flex-col gap-2">
            <div className="flex flex-wrap items-center gap-2">
              <MarkdownToolbar textareaRef={ref} value={text} onChange={setText} />
              {extra && <MarkdownInsertContext.Provider value={insert}>{extra}</MarkdownInsertContext.Provider>}
            </div>
            <Textarea ref={ref} value={text} onChange={(e) => setText(e.target.value)} onKeyDown={(e) => {
              if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); if (text.trim() && !busy) void onSubmit(text); return; }
              keys.onKeyDown(e);
            }} rows={16} spellCheck={false} aria-label={title} className="min-h-[18rem] flex-1 resize-y font-mono text-sm" />
            <p className="text-xs text-muted-foreground">{t('markdown.shortcut_hint')}</p>
          </div>
          <section aria-label={t('markdown.preview')} className="min-w-0">
            <p className="mb-2 text-sm font-medium">{t('markdown.preview')}</p>
            <div className="min-h-[18rem] overflow-y-auto rounded-md border bg-muted/20 p-3 text-sm lg:max-h-[26rem]">
              {text.trim() ? <MarkdownView text={text} /> : <p className="text-muted-foreground">{t('markdown.preview_empty')}</p>}
            </div>
          </section>
        </div>
        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)} disabled={busy}>{t('common.cancel')}</Button>
          <Button onClick={() => void onSubmit(text)} disabled={busy || !text.trim()}><Send className="mr-2 h-4 w-4" />{submitLabel}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

/// extra 里的按钮用它在光标处插入文字。
export const MarkdownInsertContext = React.createContext<(snippet: string) => void>(() => {});
