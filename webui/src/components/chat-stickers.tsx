import React from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogTitle } from '@/components/ui/dialog';
import { listStickers } from '@/lib/chat-stickers';

// Classic QQ face artwork from github.com/koishijs/QFace (public/gif).
const faces: Record<string, [string, string]> = {
  0: ['惊讶', 'Surprised'], 1: ['撇嘴', 'Pouting'], 2: ['色', 'Smitten'], 3: ['发呆', 'Dazed'],
  4: ['得意', 'Smug'], 5: ['流泪', 'Tears'], 6: ['害羞', 'Shy'], 7: ['闭嘴', 'Lips sealed'],
  8: ['睡', 'Asleep'], 9: ['大哭', 'Sobbing'], 10: ['尴尬', 'Embarrassed'], 11: ['发怒', 'Angry'],
  12: ['调皮', 'Playful'], 13: ['呲牙', 'Grinning'], 14: ['微笑', 'Smiling'],
  18: ['抓狂', 'Frustrated'], 20: ['偷笑', 'Giggle'], 21: ['可爱', 'Cute'], 32: ['疑问', 'Puzzled'],
};

export const faceUrl = (id: string) => `./assets/qq-face-s${id}.gif`;

export function Face({ id }: { id: string }) {
  const { i18n } = useTranslation();
  const name = faces[id][i18n.language.startsWith('zh') ? 0 : 1];
  return <img src={faceUrl(id)} alt={name} title={name}
    referrerPolicy="no-referrer" className="inline-block h-7 w-7 object-contain align-middle" />;
}

export function ChatText({ text }: { text: string }) {
  return <span className="whitespace-pre-wrap">{text.split(/(<emoji:\d+>)/g).map((part, i) => {
    const id = /^<emoji:(\d+)>$/.exec(part)?.[1];
    return id && faces[id] ? <Face key={i} id={id} /> : part;
  })}</span>;
}

function StickerButton({ file, onPick }: { file: File; onPick: () => void }) {
  const [url, setUrl] = React.useState('');
  React.useEffect(() => {
    const src = URL.createObjectURL(file);
    setUrl(src);
    return () => URL.revokeObjectURL(src);
  }, [file]);
  return <button type="button" className="rounded p-1 hover:bg-accent" onClick={onPick}>
    {url && <img src={url} alt={file.name} title={file.name} className="h-12 w-12 object-contain" />}
  </button>;
}

export function StickerPicker({ open, onOpenChange, onFace, onSticker, onUpload }: {
  open: boolean; onOpenChange: (open: boolean) => void; onFace: (id: string) => void;
  onSticker: (file: File) => void; onUpload: (file: File) => void;
}) {
  const { t } = useTranslation();
  const upload = React.useRef<HTMLInputElement>(null);
  const stickers = useQuery({ queryKey: ['chat-stickers'], queryFn: listStickers, enabled: open, retry: false });
  return <Dialog open={open} onOpenChange={onOpenChange}>
    <DialogContent>
      <DialogTitle>{t('chat.faces')}</DialogTitle>
      <div className="grid grid-cols-5 gap-1 sm:grid-cols-10">
        {Object.keys(faces).map((id) => <button key={id} type="button" className="rounded p-1 hover:bg-accent"
          onClick={() => { onOpenChange(false); onFace(id); }}><Face id={id} /></button>)}
      </div>
      <div className="flex items-center justify-between gap-3">
        <h3 className="font-medium">{t('chat.stickers')}</h3>
        <Button variant="outline" size="sm" onClick={() => upload.current?.click()}>{t('chat.upload_sticker')}</Button>
        <input ref={upload} type="file" accept="image/*" hidden onChange={(e) => {
          const file = e.target.files?.[0];
          e.target.value = '';
          if (file) onUpload(file);
        }} />
      </div>
      {stickers.error && <p role="alert">{t('common.failed', { error: stickers.error.message })}</p>}
      <div className="flex max-h-60 flex-wrap gap-1 overflow-y-auto">
        {stickers.data?.map((file, index) => <StickerButton key={index} file={file}
          onPick={() => { onOpenChange(false); onSticker(file); }} />)}
      </div>
    </DialogContent>
  </Dialog>;
}
