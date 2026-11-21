// 各页共用的表单小部件。

import type React from 'react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';

export function Field({ label, hint, children, id }: { label: string; hint?: React.ReactNode; children: React.ReactNode; id?: string }) {
  return (
    <div className="min-w-0 space-y-2">
      <Label htmlFor={id}>{label}</Label>
      {children}
      {hint && <p className="text-xs leading-relaxed text-muted-foreground">{hint}</p>}
    </div>
  );
}

export function Toggle({ label, hint, checked, onChange }: { label: string; hint?: string; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <div className="flex items-start justify-between gap-4">
      <div className="min-w-0">
        <p className="text-sm font-medium">{label}</p>
        {hint && <p className="mt-1 text-xs leading-relaxed text-muted-foreground">{hint}</p>}
      </div>
      <Switch checked={checked} onCheckedChange={onChange} aria-label={label} />
    </div>
  );
}

export function Choice({ value, onChange, options }: { value: string; onChange: (v: string) => void; options: { value: string; label: string }[] }) {
  return (
    <Select value={value} onValueChange={onChange}>
      <SelectTrigger><SelectValue /></SelectTrigger>
      <SelectContent>{options.map((o) => <SelectItem key={o.value} value={o.value}>{o.label}</SelectItem>)}</SelectContent>
    </Select>
  );
}

/// 数字输入：只保留数字，空串当 0。
export const digits = (value: string) => (value === '' ? 0 : Number(value.replace(/\D/g, '')));

/// 卡片网格：与 Dice!Next 平台连接页相同的自适应列宽。
export function CardGrid({ children }: { children: React.ReactNode }) {
  return <div className="grid gap-4 grid-cols-[repeat(auto-fill,minmax(min(360px,100%),1fr))]">{children}</div>;
}

export function EmptyState({ icon: Icon, title, hint, action }: { icon: React.ComponentType<{ className?: string }>; title: string; hint: string; action?: React.ReactNode }) {
  return (
    <div className="flex flex-col items-center justify-center py-16 text-center">
      <div className="mb-4 rounded-full bg-muted p-4"><Icon className="h-8 w-8 text-muted-foreground" /></div>
      <h3 className="mb-1 text-lg font-semibold">{title}</h3>
      <p className="mb-4 max-w-md text-sm text-muted-foreground">{hint}</p>
      {action}
    </div>
  );
}

/// 卡片右上角的状态点。
export function StatusDot({ tone }: { tone: 'ok' | 'warn' | 'off' }) {
  const color = tone === 'ok' ? 'bg-emerald-500' : tone === 'warn' ? 'bg-amber-500' : 'bg-muted-foreground/40';
  return <span className={`inline-block h-2.5 w-2.5 shrink-0 rounded-full ring-2 ring-card ${color}`} />;
}

export function errorText(e: unknown) {
  return e instanceof Error ? e.message : String(e);
}
