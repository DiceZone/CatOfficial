/** 文件管理的回收站（照搬 BDC）。
 *
 * 文件管理里的删除一律先进回收站，留 7 天，到期自动删除（删除时也可以选择不进回收站）。
 * 覆盖时被盖掉的那一份也在这里。可以还原、彻底删除、清空。
 */

import { useCallback, useEffect, useState } from "react";
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { formatDateTime, useToast } from '@/lib/bdc-compat';
import { useStyledDialog } from '@/components/styled-dialog';
import { File, Folder, Loader2, RotateCcw, Trash2 } from "lucide-react";

export interface TrashEntry {
  id: string;
  path: string;
  is_dir: boolean;
  /** 条目太多没量完时为 -1 */
  size: number;
  deleted_at: string;
  expires_at: string;
  deleted_by?: string;
}

type FileCall = (payload: Record<string, unknown>) => Promise<any>;

function formatSize(bytes: number): string {
  if (bytes < 0) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

/** 还剩多久被自动删除 */
function remaining(expiresAt: string, now = Date.now()): string {
  const left = new Date(expiresAt).getTime() - now;
  if (Number.isNaN(left)) return "—";
  if (left <= 0) return "即将删除";
  const hours = Math.floor(left / 3_600_000);
  if (hours >= 24) return `${Math.floor(hours / 24)} 天后`;
  if (hours >= 1) return `${hours} 小时后`;
  return `${Math.max(1, Math.floor(left / 60_000))} 分钟后`;
}

/** 出错时显示后端给的原因，没有就用默认的说法 */
export function unsupportedMessage(detail: string | undefined, fallback: string): string {
  return detail || fallback;
}

export function FileTrashDialog({ open, onOpenChange, call, onChanged }: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  call: FileCall;
  /** 还原之后，文件列表要重新读一遍 */
  onChanged: () => void;
}) {
  const styledDialog = useStyledDialog();
  const toast = useToast();
  const [entries, setEntries] = useState<TrashEntry[] | null>(null);
  const [retention, setRetention] = useState(7);
  const [busy, setBusy] = useState("");

  const load = useCallback(async () => {
    try {
      const resp = await call({ action: "trash-list" });
      setEntries(resp.entries || []);
      if (resp.retention_days) setRetention(resp.retention_days);
    } catch (err: any) {
      toast.error(unsupportedMessage(err?.body?.detail, "读取回收站失败"));
      setEntries([]);
    }
  }, [call]);

  useEffect(() => {
    if (open) {
      setEntries(null);
      void load();
    }
  }, [open, load]);

  async function restore(entry: TrashEntry, overwrite = false) {
    setBusy(`restore:${entry.id}`);
    try {
      await call({ action: "trash-restore", ids: [entry.id], overwrite });
      toast.success(`已还原到 ${entry.path}`);
      onChanged();
      await load();
    } catch (err: any) {
      const detail: string = err?.body?.detail || "";
      if (!overwrite && (err?.status === 409 || detail.startsWith("原位置已有同名项目"))) {
        setBusy("");
        const confirmed = await styledDialog.confirm({
          title: "原位置已有同名项目",
          description: `${entry.path} 已存在。覆盖后，原位置现有的那一份会移入回收站。`,
          confirmText: "覆盖并还原",
        });
        if (confirmed) await restore(entry, true);
        return;
      }
      toast.error(unsupportedMessage(detail, "还原失败"));
    } finally {
      setBusy("");
    }
  }

  async function purge(target: TrashEntry | "all") {
    const confirmed = await styledDialog.confirm({
      title: target === "all" ? "清空回收站" : `彻底删除 ${target.path}`,
      description: target === "all"
        ? "回收站中的全部项目将被永久删除，无法恢复。"
        : "该项目将被永久删除，无法恢复。",
      confirmText: "永久删除",
      destructive: true,
    });
    if (!confirmed) return;
    setBusy(target === "all" ? "empty" : `purge:${target.id}`);
    try {
      await call(target === "all" ? { action: "trash-empty" } : { action: "trash-delete", ids: [target.id] });
      toast.success(target === "all" ? "已清空回收站" : "已永久删除");
      await load();
    } catch (err: any) {
      toast.error(unsupportedMessage(err?.body?.detail, "删除失败"));
    } finally {
      setBusy("");
    }
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-4xl">
        <DialogHeader>
          <DialogTitle>回收站</DialogTitle>
          <DialogDescription>
            删除的文件会在回收站保留 {retention} 天，到期后自动永久删除。
          </DialogDescription>
        </DialogHeader>

        <div className="max-h-[60vh] overflow-auto rounded-md border border-border">
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>原位置</TableHead>
                <TableHead className="w-24">大小</TableHead>
                <TableHead className="w-40">删除时间</TableHead>
                <TableHead className="w-24">自动删除</TableHead>
                <TableHead className="w-32">操作人</TableHead>
                <TableHead className="w-28 text-right">操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {entries === null ? (
                <TableRow>
                  <TableCell colSpan={6} className="py-8 text-center text-muted-foreground">
                    <Loader2 className="mx-auto h-5 w-5 animate-spin" />
                  </TableCell>
                </TableRow>
              ) : entries.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={6} className="py-8 text-center text-sm text-muted-foreground">
                    回收站是空的
                  </TableCell>
                </TableRow>
              ) : entries.map((entry) => (
                <TableRow key={entry.id}>
                  <TableCell className="max-w-[18rem]">
                    <span className="flex items-center gap-2 break-all font-mono text-xs">
                      {entry.is_dir
                        ? <Folder className="h-4 w-4 shrink-0 text-amber-500" />
                        : <File className="h-4 w-4 shrink-0 text-muted-foreground" />}
                      /{entry.path}
                    </span>
                  </TableCell>
                  <TableCell className="text-sm text-muted-foreground">{formatSize(entry.size)}</TableCell>
                  <TableCell className="text-sm text-muted-foreground">
                    {entry.deleted_at ? formatDateTime(entry.deleted_at) : "—"}
                  </TableCell>
                  <TableCell className="text-sm text-muted-foreground">{remaining(entry.expires_at)}</TableCell>
                  <TableCell className="truncate text-sm text-muted-foreground" title={entry.deleted_by}>
                    {entry.deleted_by || "—"}
                  </TableCell>
                  <TableCell className="text-right">
                    <div className="flex justify-end gap-1">
                      <Button size="sm" variant="ghost" title="还原到原位置"
                        className="text-muted-foreground hover:bg-accent hover:text-foreground"
                        disabled={Boolean(busy)} onClick={() => void restore(entry)}>
                        {busy === `restore:${entry.id}`
                          ? <Loader2 className="h-4 w-4 animate-spin" /> : <RotateCcw className="h-4 w-4" />}
                      </Button>
                      <Button size="sm" variant="ghost" title="永久删除"
                        className="text-red-600 hover:bg-red-100 hover:text-red-700 dark:hover:bg-red-900/60 dark:hover:text-red-200"
                        disabled={Boolean(busy)} onClick={() => void purge(entry)}>
                        {busy === `purge:${entry.id}`
                          ? <Loader2 className="h-4 w-4 animate-spin" /> : <Trash2 className="h-4 w-4" />}
                      </Button>
                    </div>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </div>

        <DialogFooter className="gap-2 sm:justify-between">
          <Button size="sm" variant="destructive" disabled={Boolean(busy) || !entries?.length}
            onClick={() => void purge("all")}>
            {busy === "empty" ? <Loader2 className="mr-1 h-4 w-4 animate-spin" /> : <Trash2 className="mr-1 h-4 w-4" />}
            清空回收站
          </Button>
          <Button size="sm" variant="outline" onClick={() => onOpenChange(false)}>关闭</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
