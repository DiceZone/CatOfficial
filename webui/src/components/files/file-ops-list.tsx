/** 文件操作记录：谁、什么时候、从哪个 IP、对哪个路径做了什么（照搬 BDC）。
 *
 * 记录写在安装目录的 .catofficial/file-ops.jsonl 里，这里只是读出来看。显示在文件管理页的「操作记录」里。
 */

import { useCallback, useEffect, useMemo, useState } from "react";
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { formatDateTime, useToast } from '@/lib/bdc-compat';
import { Loader2 } from "lucide-react";
import { unsupportedMessage } from "./file-trash-dialog";

interface OpRecord {
  t: string;
  action: string;
  path?: string;
  target?: string;
  count?: number;
  bytes?: number;
  actor?: string;
  ip?: string;
  admin?: boolean;
  via?: string;
  trash_id?: string;
  permanent?: boolean;
}

type FileCall = (payload: Record<string, unknown>) => Promise<any>;

const PAGE_SIZE = 20;

function describe(record: OpRecord): string {
  switch (record.action) {
    case "delete": return record.permanent ? "直接删除" : "删除（移入回收站）";
    case "write": return "保存文件";
    case "upload": return record.trash_id ? "上传（覆盖，原文件移入回收站）" : "上传";
    case "mkdir": return "新建文件夹";
    case "rename": return record.trash_id ? "移动（覆盖，原项目移入回收站）" : "移动 / 重命名";
    case "copy": return record.trash_id ? "复制（覆盖，原项目移入回收站）" : "复制";
    case "compress": return "压缩";
    case "extract": return "解压";
    case "trash-restore": return "从回收站还原";
    case "trash-delete": return "从回收站永久删除";
    case "trash-empty": return "清空回收站";
    case "trash-expire": return "回收站到期自动删除";
    default: return record.action;
  }
}

function formatSize(bytes?: number): string {
  if (!bytes || bytes < 0) return "";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

export function FileOpsList({ call, reloadKey = 0 }: {
  call: FileCall;
  /** 每变一次就重新读一遍（外面的「刷新」按钮） */
  reloadKey?: number;
}) {
  const toast = useToast();
  const [records, setRecords] = useState<OpRecord[] | null>(null);
  const [page, setPage] = useState(1);

  const load = useCallback(async () => {
    setRecords(null);
    try {
      const resp = await call({ action: "ops", limit: 500 });
      setRecords(resp.records || []);
    } catch (err: any) {
      toast.error(unsupportedMessage(err?.body?.detail, "读取文件操作记录失败"));
      setRecords([]);
    }
  }, [call]);

  useEffect(() => {
    setPage(1);
    void load();
  }, [load, reloadKey]);

  const total = records?.length || 0;
  const pages = Math.max(1, Math.ceil(total / PAGE_SIZE));
  const visible = useMemo(
    () => (records || []).slice((page - 1) * PAGE_SIZE, page * PAGE_SIZE),
    [records, page],
  );

  return (
    <div className="space-y-4">
      <div className="overflow-x-auto">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead className="w-[11rem]">时间</TableHead>
              <TableHead className="w-[10rem]">操作人</TableHead>
              <TableHead className="w-[8rem]">IP</TableHead>
              <TableHead className="w-[12rem]">操作</TableHead>
              <TableHead>路径</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {records === null ? (
              <TableRow>
                <TableCell colSpan={5} className="py-10 text-center">
                  <Loader2 className="mx-auto h-5 w-5 animate-spin text-muted-foreground" />
                </TableCell>
              </TableRow>
            ) : records.length === 0 ? (
              <TableRow>
                <TableCell colSpan={5} className="py-10 text-center text-muted-foreground">
                  还没有记录。
                </TableCell>
              </TableRow>
            ) : visible.map((record, index) => (
              <TableRow key={`${record.t}-${index}`}>
                <TableCell className="whitespace-nowrap text-sm">
                  {record.t ? formatDateTime(record.t) : "—"}
                </TableCell>
                <TableCell className="text-sm text-muted-foreground">
                  <span className="flex flex-wrap items-center gap-1">
                    {record.actor || (record.via === "system" ? "系统记录" : "—")}
                    {record.admin && <Badge variant="outline" className="text-[10px]">管理员</Badge>}
                  </span>
                </TableCell>
                <TableCell className="font-mono text-xs text-muted-foreground">{record.ip || "—"}</TableCell>
                <TableCell className="text-sm">
                  <Badge variant={record.action === "delete" && record.permanent ? "destructive" : "outline"}>
                    {describe(record)}
                  </Badge>
                  {(record.count || record.bytes) ? (
                    <span className="mt-1 block text-xs text-muted-foreground">
                      {[record.count ? `${record.count} 项` : "", formatSize(record.bytes)].filter(Boolean).join(" · ")}
                    </span>
                  ) : null}
                </TableCell>
                <TableCell className="break-all font-mono text-xs">
                  {record.path ? `/${record.path.replace(/^\/+/, "")}` : ""}
                  {record.target ? ` → /${record.target.replace(/^\/+/, "")}` : ""}
                </TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </div>
      <div className="flex items-center justify-between">
        <span className="text-sm text-muted-foreground">共 {total} 条</span>
        <div className="flex items-center gap-2">
          <Button variant="outline" size="sm" disabled={page <= 1} onClick={() => setPage((value) => value - 1)}>
            上一页
          </Button>
          <span className="text-sm">{page} / {pages}</span>
          <Button variant="outline" size="sm" disabled={page >= pages} onClick={() => setPage((value) => value + 1)}>
            下一页
          </Button>
        </div>
      </div>
    </div>
  );
}
