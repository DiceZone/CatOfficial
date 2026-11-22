/**
 * 文件管理（照搬 BDC「骰子之间」公寓的 InstanceFiles，接到 CatOfficial 自己的 /api/webui/files）。
 *
 * 默认开放的是 CatOfficial 的安装目录（config.json、data、web 所在的那一层）；
 * 启动时加 --files-full-access 才开放整台机器。功能对齐 MCSM 的文件管理：上传下载、在线编辑、
 * 图片预览、压缩解压、剪切复制粘贴、回收站与操作记录。
 *
 * 剪切/复制走「先记下来，再到目标目录粘贴」的两段式，和资源管理器一致——
 * 一次弹窗里让人手打目标路径，比选中再走过去要难用得多。
 */

import { lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle, DialogFooter } from '@/components/ui/dialog';
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '@/components/ui/dropdown-menu';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { apiClient, formatDateTime, useToast } from '@/lib/bdc-compat';
import { useStyledDialog } from '@/components/styled-dialog';
import {
  ArrowLeft, ChevronDown, ChevronRight, Copy, Download, File, FileArchive, FileCode2,
  FilePlus, Folder, FolderPlus, Loader2, Plus, RefreshCw, Scissors,
  TextCursorInput, Trash2, Upload, Image as ImageIcon,
} from "lucide-react";
// 从不含 CodeMirror 的轻模块引入。要是从 FileEditor 引，一个静态 import
// 就会把整包 CodeMirror 拉回主 bundle，下面那层 lazy 等于白写。
import {
  DEFAULT_THEME, EDITOR_THEMES, type EditorThemeKey, languageLabel,
} from "./editor-themes";
import { uploadWithProgress } from "@/lib/upload";
import { FileTrashDialog } from "./file-trash-dialog";

// 编辑器连着 CodeMirror 和几套主题，几百 KB。不打开它的人不该为此付出首屏体积。
const FileEditor = lazy(() =>
  import("./file-editor").then((module) => ({ default: module.FileEditor })),
);

//: 编辑器主题记在浏览器本地。这是纯粹的个人口味，不值得占一行用户设置。
const THEME_STORAGE_KEY = "catofficial.file-editor.theme";

//: 每页显示多少个。默认给小值：一个 data 目录动辄几十上百个文件，
//: 一次全铺出来要滚很久才找得到东西。
const PAGE_SIZES = [10, 25, 50, 100] as const;

//: 进度轮询间隔。压缩几百 MB 要几分钟，一秒一次既跟得上又不糊请求。
const JOB_POLL_MS = 1000;

//: 解压时可选的条目名编码。
//:
//: Windows 自带的压缩工具按本地代码页写文件名，当 UTF-8 读会得到一串乱码，
//: 解出来根本认不出是什么牌堆。
const ENCODINGS = [
  { value: "utf8", label: "UTF-8（默认）" },
  { value: "gbk", label: "GBK（Windows 压缩的包）" },
];

interface FileEntry {
  name: string;
  is_dir: boolean;
  size: number;
  modified: string;
}

/** 一个正在跑的后台任务（压缩/解压）。 */
interface FileJob {
  id: string;
  kind: string;
  label: string;
  percent: number;
  current: string;
  status: string;
  error: string;
}

/** 剪贴板：记住是哪个目录里的哪些项，以及是剪还是复制。 */
interface Clipboard {
  mode: "cut" | "copy";
  dir: string;
  names: string[];
}

//: 超过这个大小就不给在线编辑了。CodeMirror 扛得住几 MB，但整份内容要先
//: base64 编解码再塞进内存，几 MB 的日志会让浏览器卡一下——而那种文件本来
//: 就该下载着看，不该在网页里翻。
const MAX_EDIT_BYTES = 1 << 20;

//: 一眼能看出是文本的扩展名。判断错了也不致命——真打开是乱码，关掉就行。
const TEXT_EXTENSIONS = [
  ".txt", ".json", ".yaml", ".yml", ".js", ".ts", ".lua", ".py", ".md",
  ".log", ".ini", ".conf", ".toml", ".xml", ".csv", ".html", ".css", ".sh", ".env",
];

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function isTextFile(name: string): boolean {
  const lower = name.toLowerCase();
  return TEXT_EXTENSIONS.some((extension) => lower.endsWith(extension));
}

function isArchive(name: string): boolean {
  return name.toLowerCase().endsWith(".zip");
}

/** 把目录和文件名拼成一条干净的路径，避免出现 `//`。 */
function join(dir: string, name: string): string {
  return `${dir.replace(/\/+$/, "")}/${name}`.replace(/^\/+/, "/");
}

//: 能在线预览的图片格式。
//:
//: 只列浏览器原生认得的：这里是直接塞进 <img> 的，认不出的格式会显示成裂图，
//: 不如让它老老实实走下载。
const PREVIEWABLE_IMAGES = new Set(["png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "avif"]);

//: 预览的大小上限。
//:
//: 图片要整个读进内存再转成 data URL，比原始体积还大三分之一。二维码和插图
//: 都在几百 KB 以内，二十兆足够，再大就该下载下来看。
const MAX_PREVIEW_BYTES = 20 * 1024 * 1024;

/** 这个文件能不能预览。 */
function isPreviewableImage(name: string): boolean {
  const dot = name.lastIndexOf(".");
  if (dot < 0) return false;
  return PREVIEWABLE_IMAGES.has(name.slice(dot + 1).toLowerCase());
}

/** 按扩展名猜 MIME。猜不出就交给浏览器自己嗅探。 */
function imageMime(name: string): string {
  const ext = name.slice(name.lastIndexOf(".") + 1).toLowerCase();
  if (ext === "svg") return "image/svg+xml";
  if (ext === "ico") return "image/x-icon";
  if (ext === "jpg") return "image/jpeg";
  return `image/${ext}`;
}

/** base64 ↔ 字节。分块处理，一次 apply 整个数组会在大文件上爆栈。 */
function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 8192) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
  }
  return btoa(binary);
}

function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value || "");
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

export function FileManager({
  openPath,
  isAdmin = true,
  fullAccess = false,
}: {
  /** 面板只有一个登录身份，等同管理员：删除时可以选择不放入回收站、直接删除 */
  isAdmin?: boolean;
  /** 是否开放了整台机器（启动参数 --files-full-access） */
  fullAccess?: boolean;
  /** 从别处点「打开某个文件夹」过来。seq 每点一次加一，同一个路径点第二次也会再打开 */
  openPath?: { path: string; seq: number };
}) {
  const styledDialog = useStyledDialog();
  const fileInput = useRef<HTMLInputElement>(null);

  const [path, setPath] = useState(openPath?.path || "/");
  useEffect(() => {
    if (openPath) setPath(openPath.path);
  }, [openPath?.seq]);
  const [entries, setEntries] = useState<FileEntry[] | null>(null);
  const toast = useToast();
  const [busy, setBusy] = useState("");
  //: 上传进度百分比；null = 没有正在进行的上传。
  //: 传大牌堆时界面上只有一个不动的转圈，人分不清是在传还是卡死了，
  //: 然后就去点第二次。
  const [uploadPercent, setUploadPercent] = useState<number | null>(null);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [clipboard, setClipboard] = useState<Clipboard | null>(null);
  // 后台任务。刻意不进 busy —— 压缩时还能继续浏览、上传、改别的文件。
  const [jobs, setJobs] = useState<FileJob[]>([]);
  // 解压前的设置：目标目录 + 编码
  const [preview, setPreview] = useState<{ name: string; url: string } | null>(null);
  const [extracting, setExtracting] = useState<{ name: string; target: string; encoding: string } | null>(null);
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState<number>(10);
  const [trashOpen, setTrashOpen] = useState(false);
  //: 管理员删除前的确认：删哪几项、是否不放入回收站
  const [deleting, setDeleting] = useState<string[] | null>(null);
  const [deletePermanent, setDeletePermanent] = useState(false);

  // 路径导览：平时是面包屑，点空白处就地变成输入框
  const [editingPath, setEditingPath] = useState(false);
  const [pathDraft, setPathDraft] = useState("/");

  // 在线编辑
  const [editor, setEditor] = useState<{ name: string; content: string } | null>(null);
  const [editorDirty, setEditorDirty] = useState(false);
  const [theme, setTheme] = useState<EditorThemeKey>(() => {
    const saved = localStorage.getItem(THEME_STORAGE_KEY) as EditorThemeKey | null;
    return saved && saved in EDITOR_THEMES ? saved : DEFAULT_THEME;
  });

  /** 文件操作：一律经面板接口（POST /api/webui/files，body 是 {action, ...}）。 */
  const call = useCallback(
    (payload: Record<string, unknown>) => apiClient.post<any>("/api/webui/files", payload),
    [],
  );

  const load = useCallback(async () => {
    setSelected(new Set());
    try {
      const resp = await call({ action: "list", path });
      setEntries(resp.entries || []);
    } catch (err: any) {
      toast.error(err?.body?.detail || "读取目录失败");
      setEntries([]);
    }
  }, [call, path]);

  useEffect(() => { void load(); }, [load]);
  useEffect(() => { setPathDraft(path); }, [path]);
  // 换目录还停在第 5 页会看到一片空白，让人以为目录是空的
  useEffect(() => { setPage(1); }, [path]);

  const segments = useMemo(() => path.split("/").filter(Boolean), [path]);

  const totalPages = Math.max(1, Math.ceil((entries?.length || 0) / pageSize));
  // 删掉最后一页的内容之后页码会越界，夹回来而不是显示空白
  const currentPage = Math.min(page, totalPages);
  const visible = useMemo(
    () => (entries || []).slice((currentPage - 1) * pageSize, currentPage * pageSize),
    [entries, currentPage, pageSize],
  );
  // 表格至少占「默认每页条数 + 1」行（多的一行留给「返回上一级」），不够的用空行补齐。
  // 目录里只有两三个文件时整块也不缩，翻目录、切页时下面的东西不会跟着上下跳。
  const fillerRows = Math.max(0, PAGE_SIZES[0] + 1 - ((segments.length > 0 ? 1 : 0) + (entries === null || entries.length === 0 ? 1 : 0) + visible.length));

  /** 盯一个后台任务直到它结束。

      不占 busy：压缩一个大目录要几分钟，这期间用户完全可以去别的目录看东西。 */
  const watchJob = useCallback((id: string, kind: string, label: string) => {
    setJobs((previous) => [
      ...previous,
      { id, kind, label, percent: 0, current: "", status: "running", error: "" },
    ]);

    // 自己排下一轮，而不是 setInterval：压缩大目录时机器正忙，一次查询可能
    // 比一个周期还久。到点就发的话请求会堆起来，把浏览器的连接占满，这一页
    // 上别的操作全排在后面。
    let stopped = false;
    const poll = async () => {
      if (stopped) return;
      let snapshot: any = null;
      try {
        snapshot = await apiClient.get<any>(
          `/api/webui/files/jobs/${id}`,
        );
      } catch {
        // 单次查询失败不代表任务挂了（可能只是忙），下一轮接着问
        setTimeout(() => void poll(), JOB_POLL_MS);
        return;
      }
      const status = String(snapshot.status || "running");
      setJobs((previous) => previous.map((item) => (
        item.id === id
          ? {
              ...item,
              percent: Number(snapshot.percent || 0),
              current: String(snapshot.current || ""),
              status,
              error: String(snapshot.error || ""),
            }
          : item
      )));

      if (status === "running") {
        setTimeout(() => void poll(), JOB_POLL_MS);
        return;
      }
      stopped = true;
      // 结果落在文件系统上，刷新目录才看得到
      void load();
      // 成功的任务过几秒自己消失；失败的留着，让人看得到原因
      if (status === "done") {
        setTimeout(() => {
          setJobs((previous) => previous.filter((item) => item.id !== id));
        }, 4000);
      }
    };
    setTimeout(() => void poll(), JOB_POLL_MS);
  }, [load]);

  /** 撞名时问一句要不要覆盖。返回 null 表示用户取消。 */
  async function confirmOverwrite(
    payload: Record<string, unknown>, what: string,
  ): Promise<boolean | null> {
    let conflicts: string[] = [];
    try {
      const resp = await call({ action: "peek", ...payload });
      conflicts = resp.conflicts || [];
    } catch {
      // 预检失败不该拦住正常操作，按「没撞上」继续，真撞了后端还会拒
      return false;
    }
    if (conflicts.length === 0) return false;

    const preview = conflicts.slice(0, 5).join("、");
    const more = conflicts.length > 5 ? ` 等 ${conflicts.length} 项` : "";
    const confirmed = await styledDialog.confirm({
      title: `${what}会覆盖已有内容`,
      description: `已存在：${preview}${more}。覆盖之后原来的内容找不回来。`,
      confirmText: "覆盖",
    });
    return confirmed ? true : null;
  }

  /** 统一的「做一件事然后刷新」，省掉每个操作里重复的 try/finally。 */
  async function run(key: string, failure: string, action: () => Promise<unknown>) {
    setBusy(key);
    try {
      await action();
      await load();
    } catch (err: any) {
      toast.error(err?.body?.detail || failure);
    } finally {
      setBusy("");
    }
  }

  function toggle(name: string) {
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      return next;
    });
  }

  // ---- 单项操作 ----

  function download(entry: FileEntry) {
    // 浏览器自己下载（带着登录 Cookie），不经 base64 读进内存：几百 MB 的日志也能下
    const anchor = document.createElement("a");
    anchor.href = `./api/webui/files/download?${new URLSearchParams({ path: join(path, entry.name) })}`;
    anchor.download = entry.name;
    anchor.click();
  }

  async function openPreview(entry: FileEntry) {
    if (entry.size > MAX_PREVIEW_BYTES) {
      await styledDialog.alert({
        title: "图片过大",
        description: `超过 ${formatSize(MAX_PREVIEW_BYTES)}，请下载后查看。`,
      });
      return;
    }
    setBusy(`preview:${entry.name}`);
    try {
      const resp = await call({ action: "read", path: join(path, entry.name) });
      // 用 data URL 而不是 blob URL：预览关掉就没了，不必再管释放；
      // 几百 KB 的图片这点开销无所谓。
      setPreview({
        name: entry.name,
        url: `data:${imageMime(entry.name)};base64,${resp.content_base64}`,
      });
    } catch (err: any) {
      toast.error(err?.body?.detail || "读取图片失败");
    } finally {
      setBusy("");
    }
  }

  async function openEditor(entry: FileEntry) {
    if (entry.size > MAX_EDIT_BYTES) {
      await styledDialog.alert({
        title: "文件过大",
        description: `超过 ${formatSize(MAX_EDIT_BYTES)}，请下载后编辑。`,
      });
      return;
    }
    setBusy(`edit:${entry.name}`);
    try {
      const resp = await call({ action: "read", path: join(path, entry.name) });
      const text = new TextDecoder("utf-8").decode(base64ToBytes(resp.content_base64));
      setEditor({ name: entry.name, content: text });
      setEditorDirty(false);
    } catch (err: any) {
      toast.error(err?.body?.detail || "读取文件失败");
    } finally {
      setBusy("");
    }
  }

  async function saveEditor() {
    if (!editor) return;
    const encoded = bytesToBase64(new TextEncoder().encode(editor.content));
    await run("save", "保存失败", async () => {
      await call({ action: "write", path: join(path, editor.name), content_base64: encoded });
      setEditor(null);
      setEditorDirty(false);
    });
  }

  async function upload(file: File) {
    const overwrite = await confirmOverwrite(
      { path: "", target: join(path, file.name) }, "上传",
    );
    if (overwrite === null) {
      if (fileInput.current) fileInput.current.value = "";
      return;
    }
    const target = join(path, file.name);
    await run("upload", "上传失败", async () => {
      setUploadPercent(0);
      try {
        // 原样直传：正文就是文件本身，所以内存占用和文件大小无关，而且 XHR 能报进度
        const resp = await uploadWithProgress({
          url: `./api/webui/files/upload?${new URLSearchParams({ path: target })}`,
          body: file,
          onProgress: (state) => setUploadPercent(state.percent),
        });
        const result = resp.json || {};
        if (resp.status >= 400 || result.error) {
          throw { body: { detail: result.error || `上传失败（HTTP ${resp.status}）` } };
        }
      } finally {
        setUploadPercent(null);
        if (fileInput.current) fileInput.current.value = "";
      }
    });
  }

  async function rename(entry: FileEntry) {
    const name = await styledDialog.prompt({
      title: `重命名 ${entry.name}`,
      description: "",
      defaultValue: entry.name,
      confirmText: "重命名",
    });
    if (!name || name === entry.name) return;
    await run(`rename:${entry.name}`, "重命名失败", () => call({
      action: "rename", path: join(path, entry.name), target: join(path, name),
    }));
  }

  /** 打开解压设置。默认解到和压缩包同名的目录里：包里如果是一堆散文件，
      直接铺到当前目录会把它搅乱，而且没法撤销。 */
  function openExtract(entry: FileEntry) {
    setExtracting({
      name: entry.name,
      target: entry.name.replace(/\.zip$/i, ""),
      encoding: "utf8",
    });
  }

  async function startExtract() {
    if (!extracting) return;
    const archive = join(path, extracting.name);
    // 目标留空就解到当前目录
    const folder = extracting.target.trim();
    const target = folder ? join(path, folder) : path;
    const encoding = extracting.encoding === "gbk" ? "gbk" : "";

    const overwrite = await confirmOverwrite(
      { path: archive, target, encoding }, "解压",
    );
    if (overwrite === null) return;

    setExtracting(null);
    try {
      const resp = await call({
        action: "extract", path: archive, target, encoding, overwrite,
      });
      if (resp.job?.id) watchJob(resp.job.id, "extract", `解压 ${extracting.name}`);
    } catch (err: any) {
      toast.error(err?.body?.detail || "解压失败");
    }
  }

  // ---- 批量操作 ----

  const chosen = useMemo(() => Array.from(selected), [selected]);

  /** 删除：一律先进回收站（保留 7 天，可还原），所以不再逐次确认。
      管理员要多确认一次，并可选择不放入回收站、直接删除。 */
  async function performDelete(names: string[], permanent: boolean) {
    const key = names.length === 1 && !selected.has(names[0]) ? `del:${names[0]}` : "delete";
    let failed = false;
    await run(key, "删除失败", async () => {
      try {
        for (const name of names) {
          const entry = (entries || []).find((item) => item.name === name);
          await call({
            action: "delete",
            path: join(path, name),
            // 目录连内容一起删：进的是回收站，整个目录原样留着
            recursive: Boolean(entry?.is_dir),
            ...(permanent ? { permanent: true } : {}),
          });
        }
      } catch (err) {
        failed = true;
        throw err;
      }
    });
    if (!failed) {
      toast.success(permanent
        ? `已永久删除 ${names.length} 项`
        : `已将 ${names.length} 项移入回收站，7 天内可还原`);
    }
  }

  /** 单项删除。和批量删除同一套语义，只是不用先勾选。 */
  async function removeOne(entry: FileEntry) {
    if (isAdmin) {
      setDeletePermanent(false);
      setDeleting([entry.name]);
      return;
    }
    await performDelete([entry.name], false);
  }

  async function removeSelected() {
    if (isAdmin) {
      setDeletePermanent(false);
      setDeleting(chosen);
      return;
    }
    await performDelete(chosen, false);
  }

  async function paste() {
    if (!clipboard) return;
    if (clipboard.dir === path && clipboard.mode === "cut") {
      setClipboard(null);
      return; // 剪到原地等于什么都没做
    }
    // 逐项预检：只要有一项撞上就问一次，答复对整批生效
    let overwrite = false;
    for (const name of clipboard.names) {
      const answer = await confirmOverwrite({ path: "", target: join(path, name) }, "粘贴");
      if (answer === null) return;
      if (answer) {
        overwrite = true;
        break;
      }
    }

    await run("paste", "粘贴失败", async () => {
      for (const name of clipboard.names) {
        await call({
          action: clipboard.mode === "cut" ? "rename" : "copy",
          path: join(clipboard.dir, name),
          target: join(path, name),
          overwrite,
        });
      }
      setClipboard(null);
    });
  }

  async function compressSelected() {
    const suggested = chosen.length === 1 ? `${chosen[0]}.zip` : "archive.zip";
    const name = await styledDialog.prompt({
      title: `压缩 ${chosen.length} 项`,
      description: "打包成一个 zip 放在当前目录。",
      defaultValue: suggested,
      confirmText: "压缩",
    });
    if (!name) return;
    const target = join(path, name.endsWith(".zip") ? name : `${name}.zip`);
    const overwrite = await confirmOverwrite({ path: "", target }, "压缩");
    if (overwrite === null) return;
    setSelected(new Set());
    try {
      const resp = await call({
        action: "compress",
        items: chosen.map((item) => join(path, item)),
        target, overwrite,
      });
      if (resp.job?.id) watchJob(resp.job.id, "compress", `压缩 ${chosen.length} 项`);
    } catch (err: any) {
      toast.error(err?.body?.detail || "压缩失败");
    }
  }

  async function mkdir() {
    const name = await styledDialog.prompt({
      title: "新建文件夹", description: "", confirmText: "创建",
    });
    if (!name) return;
    await run("mkdir", "创建失败", () => call({ action: "mkdir", path: join(path, name) }));
  }

  async function newFile() {
    const name = await styledDialog.prompt({
      title: "新建文件", description: "", confirmText: "创建",
    });
    if (!name) return;
    await run("newfile", "创建失败", () => call({
      action: "write", path: join(path, name), content_base64: "",
    }));
  }

  const anyBusy = Boolean(busy);

  return (
    <Card className="border-border">
      <CardHeader>
        <div className="flex flex-wrap items-start justify-between gap-3">
          <div>
            <CardTitle className="flex items-center gap-2">
              <Folder className="h-5 w-5" />文件管理
            </CardTitle>
            <CardDescription>
              {fullAccess ? "已开放整台机器（启动参数 --files-full-access）" : "CatOfficial 的安装目录"}
            </CardDescription>
          </div>
          <div className="flex flex-wrap gap-2">
            <Button size="sm" variant="outline" onClick={() => void load()} disabled={anyBusy}>
              <RefreshCw className="mr-1 h-4 w-4" />刷新
            </Button>
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button size="sm" variant="outline" disabled={anyBusy}>
                  <Plus className="mr-1 h-4 w-4" />新建<ChevronDown className="ml-1 h-3.5 w-3.5" />
                </Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent align="end" className="w-36">
                <DropdownMenuItem onSelect={() => void newFile()}>
                  <FilePlus className="mr-2 h-4 w-4" />新建文件
                </DropdownMenuItem>
                <DropdownMenuItem onSelect={() => void mkdir()}>
                  <FolderPlus className="mr-2 h-4 w-4" />新建文件夹
                </DropdownMenuItem>
              </DropdownMenuContent>
            </DropdownMenu>
            <Button size="sm" variant="outline" onClick={() => setTrashOpen(true)}>
              <Trash2 className="mr-1 h-4 w-4" />回收站
            </Button>
            <Button size="sm" onClick={() => fileInput.current?.click()} disabled={anyBusy}>
              {busy === "upload" ? <Loader2 className="mr-1 h-4 w-4 animate-spin" /> : <Upload className="mr-1 h-4 w-4" />}
              {uploadPercent === null ? "上传" : `上传 ${uploadPercent}%`}
            </Button>
            <input
              ref={fileInput} type="file" className="hidden"
              onChange={(e) => { const file = e.target.files?.[0]; if (file) void upload(file); }}
            />
          </div>
        </div>
      </CardHeader>

      {/* 文件管理区做成一块独立的灰色面板。
          两边都和周围拉开，一眼就知道「这一块是文件系统」，和上面那些表单
          不是一回事。用中灰而不是近黑：浅色模式下一大片近黑太压人，而灰底
          配浅色文字的对比度已经足够。 */}
      <CardContent>
        <div className="space-y-3 rounded-lg border border-border bg-muted/40 p-3 text-foreground">
        {/* 路径导览：点段落跳转，点空白处直接输入路径 */}
        {editingPath ? (
          <Input
            autoFocus
            value={pathDraft}
            onChange={(e) => setPathDraft(e.target.value)}
            onBlur={() => { setEditingPath(false); setPathDraft(path); }}
            onKeyDown={(e) => {
              if (e.key === "Enter") {
                setPath(pathDraft.trim() || "/");
                setEditingPath(false);
              } else if (e.key === "Escape") {
                setEditingPath(false);
                setPathDraft(path);
              }
            }}
            className="border-border bg-background font-mono text-sm text-foreground"
          />
        ) : (
          <div
            className="flex min-h-9 cursor-text flex-wrap items-center gap-1 rounded-md border border-border bg-background px-3 py-1.5 text-sm"
            // 进编辑态时按当前目录填。pathDraft 只在失焦时同步的话，
            // 进了子目录再点开，框里是上一次留下的旧路径。
            onClick={() => { setPathDraft(path); setEditingPath(true); }}
            title="点击空白处可直接输入路径"
          >
            <button
              type="button"
              className="rounded-sm px-1 font-medium hover:bg-accent"
              onClick={(e) => { e.stopPropagation(); setPath("/"); }}
            >
              {fullAccess ? "根目录" : "安装目录"}
            </button>
            {segments.map((segment, index) => (
              <span key={`${segment}-${index}`} className="flex items-center gap-1">
                <ChevronRight className="h-3.5 w-3.5 text-muted-foreground" />
                <button
                  type="button"
                  className="rounded-sm px-1 hover:bg-accent"
                  onClick={(e) => {
                    e.stopPropagation();
                    setPath(`/${segments.slice(0, index + 1).join("/")}`);
                  }}
                >
                  {segment}
                </button>
              </span>
            ))}
            <span className="ml-auto text-xs text-muted-foreground">点击此处输入路径</span>
          </div>
        )}

        {/* 选中后才出现的一条操作栏，平时不占地方 */}
        {(chosen.length > 0 || clipboard) && (
          <div className="flex flex-wrap items-center gap-2 rounded-md border border-border bg-background px-3 py-2">
            {chosen.length > 0 && (
              <>
                <span className="text-sm text-muted-foreground">已选 {chosen.length} 项</span>
                <Button size="sm" variant="outline" className="border-border bg-background text-foreground hover:bg-accent" disabled={anyBusy}
                  onClick={() => { setClipboard({ mode: "cut", dir: path, names: chosen }); setSelected(new Set()); }}>
                  <Scissors className="mr-1 h-4 w-4" />剪切
                </Button>
                <Button size="sm" variant="outline" className="border-border bg-background text-foreground hover:bg-accent" disabled={anyBusy}
                  onClick={() => { setClipboard({ mode: "copy", dir: path, names: chosen }); setSelected(new Set()); }}>
                  <Copy className="mr-1 h-4 w-4" />复制
                </Button>
                <Button size="sm" variant="outline" className="border-border bg-background text-foreground hover:bg-accent" disabled={anyBusy} onClick={() => void compressSelected()}>
                  {busy === "compress" ? <Loader2 className="mr-1 h-4 w-4 animate-spin" /> : <FileArchive className="mr-1 h-4 w-4" />}
                  压缩
                </Button>
                <Button size="sm" variant="destructive" disabled={anyBusy} onClick={() => void removeSelected()}>
                  {busy === "delete" ? <Loader2 className="mr-1 h-4 w-4 animate-spin" /> : <Trash2 className="mr-1 h-4 w-4" />}
                  删除
                </Button>
              </>
            )}
            {clipboard && (
              <div className="ml-auto flex items-center gap-2">
                <Badge variant="outline">
                  {clipboard.mode === "cut" ? "已剪切" : "已复制"} {clipboard.names.length} 项
                </Badge>
                <Button size="sm" className="text-white" disabled={anyBusy} onClick={() => void paste()}>
                  {busy === "paste" ? <Loader2 className="mr-1 h-4 w-4 animate-spin" /> : null}
                  粘贴到此处
                </Button>
                <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" onClick={() => setClipboard(null)}>取消</Button>
              </div>
            )}
          </div>
        )}


        {/* 后台任务。压缩/解压期间这块显示进度，但不挡着继续操作别的。 */}
        {jobs.map((job) => (
          <div key={job.id} className="rounded-md border border-border bg-background p-2.5">
            <div className="flex items-center justify-between gap-3 text-xs">
              <span className="flex items-center gap-2">
                {job.status === "running" && <Loader2 className="h-3.5 w-3.5 animate-spin" />}
                {job.status === "done" && <span className="text-emerald-400">✓</span>}
                {(job.status === "failed" || job.status === "expired") && (
                  <span className="text-red-400">✕</span>
                )}
                {job.label}
              </span>
              <span className="text-muted-foreground">
                {job.status === "running" ? `${job.percent}%` : ""}
              </span>
            </div>
            {job.status === "running" && (
              <>
                <div className="mt-1.5 h-1.5 overflow-hidden rounded-full bg-muted">
                  <div
                    className="h-full rounded-full bg-sky-500 transition-all"
                    style={{ width: `${job.percent}%` }}
                  />
                </div>
                {job.current && (
                  <div className="mt-1 truncate font-mono text-3xs text-muted-foreground">
                    {job.current}
                  </div>
                )}
              </>
            )}
            {job.error && <div className="mt-1 text-2xs text-red-400">{job.error}</div>}
          </div>
        ))}

        <Table>
          <TableHeader>
            <TableRow className="border-border hover:bg-transparent">
              <TableHead className="w-10" />
              <TableHead className="text-muted-foreground">名称</TableHead>
              <TableHead className="w-28 text-muted-foreground">大小</TableHead>
              <TableHead className="w-44 text-muted-foreground">修改时间</TableHead>
              <TableHead className="w-56 text-right text-muted-foreground">操作</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {segments.length > 0 && (
              <TableRow className="h-14 border-border hover:bg-accent/60">
                <TableCell />
                <TableCell colSpan={4}>
                  <button
                    type="button"
                    className="flex items-center gap-2 text-sm text-muted-foreground hover:underline"
                    onClick={() => setPath(`/${segments.slice(0, -1).join("/")}`)}
                  >
                    <ArrowLeft className="h-4 w-4" />返回上一级
                  </button>
                </TableCell>
              </TableRow>
            )}
            {entries === null && (
              <TableRow className="h-14 border-border hover:bg-transparent"><TableCell colSpan={5}>
                <Loader2 className="h-4 w-4 animate-spin text-muted-foreground" />
              </TableCell></TableRow>
            )}
            {entries?.length === 0 && (
              <TableRow className="h-14 border-border hover:bg-transparent"><TableCell colSpan={5} className="text-sm text-muted-foreground">
                这个目录是空的
              </TableCell></TableRow>
            )}
            {visible.map((entry) => (
              <TableRow key={entry.name} className="h-14 border-border hover:bg-accent/60">
                <TableCell>
                  <input
                    type="checkbox"
                    className="accent-sky-500"
                    checked={selected.has(entry.name)}
                    onChange={() => toggle(entry.name)}
                  />
                </TableCell>
                <TableCell>
                  {entry.is_dir ? (
                    <button
                      type="button"
                      className="flex items-center gap-2 text-sm text-muted-foreground hover:underline"
                      onClick={() => setPath(join(path, entry.name))}
                    >
                      <Folder className="h-4 w-4 text-sky-600 dark:text-sky-400" />{entry.name}
                    </button>
                  ) : (
                    <span className="flex items-center gap-2 text-sm">
                      <File className="h-4 w-4 text-muted-foreground" />{entry.name}
                    </span>
                  )}
                </TableCell>
                <TableCell className="text-sm text-muted-foreground">
                  {entry.is_dir ? "—" : formatSize(entry.size)}
                </TableCell>
                <TableCell className="text-sm text-muted-foreground">
                  {entry.modified ? formatDateTime(entry.modified) : "—"}
                </TableCell>
                <TableCell className="text-right">
                  <div className="flex justify-end gap-1">
                    {!entry.is_dir && isPreviewableImage(entry.name) && (
                      <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" title="预览图片"
                        disabled={anyBusy} onClick={() => void openPreview(entry)}>
                        {busy === `preview:${entry.name}`
                          ? <Loader2 className="h-4 w-4 animate-spin" /> : <ImageIcon className="h-4 w-4" />}
                      </Button>
                    )}
                    {!entry.is_dir && isTextFile(entry.name) && (
                      <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" title="编辑内容"
                        disabled={anyBusy} onClick={() => void openEditor(entry)}>
                        {busy === `edit:${entry.name}`
                          ? <Loader2 className="h-4 w-4 animate-spin" /> : <FileCode2 className="h-4 w-4" />}
                      </Button>
                    )}
                    {!entry.is_dir && isArchive(entry.name) && (
                      <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" title="解压到同名目录"
                        disabled={anyBusy} onClick={() => openExtract(entry)}>
                        <FileArchive className="h-4 w-4" />
                      </Button>
                    )}
                    {!entry.is_dir && (
                      <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" title="下载"
                        disabled={anyBusy} onClick={() => void download(entry)}>
                        {busy === `get:${entry.name}`
                          ? <Loader2 className="h-4 w-4 animate-spin" /> : <Download className="h-4 w-4" />}
                      </Button>
                    )}
                    <Button size="sm" variant="ghost" className="text-muted-foreground hover:bg-accent hover:text-foreground" title="重命名"
                      disabled={anyBusy} onClick={() => void rename(entry)}>
                      <TextCursorInput className="h-4 w-4" />
                    </Button>
                    <Button size="sm" variant="ghost" className="text-red-600 hover:bg-red-100 hover:text-red-700 dark:hover:bg-red-900/60 dark:hover:text-red-200" title="删除"
                      disabled={anyBusy} onClick={() => void removeOne(entry)}>
                      {busy === `del:${entry.name}`
                        ? <Loader2 className="h-4 w-4 animate-spin" /> : <Trash2 className="h-4 w-4" />}
                    </Button>
                  </div>
                </TableCell>
              </TableRow>
            ))}
            {Array.from({ length: fillerRows }, (_, index) => (
              <TableRow key={`filler-${index}`} aria-hidden="true" className="h-14 border-transparent hover:bg-transparent">
                <TableCell colSpan={5} />
              </TableRow>
            ))}
          </TableBody>
        </Table>

        {/* 分页条常驻：条目不满一页也留着，整块高度才不会变 */}
        {(
          <div className="flex flex-wrap items-center justify-between gap-2 border-t border-border pt-2 text-xs text-muted-foreground">
            <span>
              共 {entries?.length} 项，第 {currentPage} / {totalPages} 页
            </span>
            <div className="flex items-center gap-2">
              <Select
                value={String(pageSize)}
                onValueChange={(value) => { setPageSize(Number(value)); setPage(1); }}
              >
                <SelectTrigger className="h-7 w-24 border-border bg-background text-xs text-foreground">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {PAGE_SIZES.map((size) => (
                    <SelectItem key={size} value={String(size)}>每页 {size}</SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <Button size="sm" variant="ghost" className="h-7 text-muted-foreground hover:bg-accent hover:text-foreground"
                disabled={currentPage <= 1} onClick={() => setPage(currentPage - 1)}>
                上一页
              </Button>
              <Button size="sm" variant="ghost" className="h-7 text-muted-foreground hover:bg-accent hover:text-foreground"
                disabled={currentPage >= totalPages} onClick={() => setPage(currentPage + 1)}>
                下一页
              </Button>
            </div>
          </div>
        )}
        </div>
      </CardContent>

      {/* 解压设置 */}
      <Dialog open={preview !== null} onOpenChange={(open) => { if (!open) setPreview(null); }}>
        <DialogContent className="max-h-[90vh] overflow-auto sm:max-w-4xl">
          <DialogHeader>
            <DialogTitle className="break-all font-mono text-sm">{preview?.name}</DialogTitle>
          </DialogHeader>
          {preview && (
            /* 棋盘底：透明的 PNG 直接放在白底或黑底上都看不出哪块是透明的 */
            <div className="flex justify-center rounded-md bg-[repeating-conic-gradient(#f3f4f6_0%_25%,#ffffff_0%_50%)] bg-[length:16px_16px] p-3 dark:bg-[repeating-conic-gradient(#1f2937_0%_25%,#111827_0%_50%)]">
              <img src={preview.url} alt={preview.name} className="max-h-[70vh] max-w-full object-contain" />
            </div>
          )}
          <DialogFooter>
            <Button type="button" variant="outline" onClick={() => setPreview(null)}>关闭</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <Dialog open={Boolean(extracting)} onOpenChange={(open) => { if (!open) setExtracting(null); }}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle className="font-mono text-sm">解压 {extracting?.name}</DialogTitle>
          </DialogHeader>
          <div className="space-y-3">
            <div className="space-y-1">
              <Label>解压到</Label>
              <Input
                value={extracting?.target ?? ""}
                placeholder="留空表示解压到当前目录"
                onChange={(e) => setExtracting(
                  extracting ? { ...extracting, target: e.target.value } : extracting,
                )}
              />
              <p className="text-xs text-muted-foreground">
                留空则解到当前目录。
              </p>
            </div>
            <div className="space-y-1">
              <Label>文件名编码</Label>
              <Select
                value={extracting?.encoding ?? "utf8"}
                onValueChange={(value) => setExtracting(
                  extracting ? { ...extracting, encoding: value } : extracting,
                )}
              >
                <SelectTrigger><SelectValue /></SelectTrigger>
                <SelectContent>
                  {ENCODINGS.map((item) => (
                    <SelectItem key={item.value} value={item.value}>{item.label}</SelectItem>
                  ))}
                </SelectContent>
              </Select>
              <p className="text-xs text-muted-foreground">
                文件名乱码时改用 GBK。
              </p>
            </div>
          </div>
          <DialogFooter>
            <Button variant="ghost" onClick={() => setExtracting(null)}>取消</Button>
            <Button onClick={() => void startExtract()}>开始解压</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 在线编辑 */}
      <Dialog
        open={Boolean(editor)}
        onOpenChange={async (open) => {
          if (open) return;
          if (editorDirty) {
            const discard = await styledDialog.confirm({
              title: "放弃修改？", description: "未保存的改动会丢失。", confirmText: "放弃",
            });
            if (!discard) return;
          }
          setEditor(null);
          setEditorDirty(false);
        }}
      >
        <DialogContent className="max-w-5xl">
          <DialogHeader>
            <div className="flex flex-wrap items-center justify-between gap-3 pr-8">
              <div>
                <DialogTitle className="font-mono text-sm">{editor?.name}</DialogTitle>
                <p className="mt-0.5 text-xs text-muted-foreground">
                  {editor ? languageLabel(editor.name) : ""}
                  {editorDirty && " · 未保存"}
                </p>
              </div>
              <Select
                value={theme}
                onValueChange={(value) => {
                  setTheme(value as EditorThemeKey);
                  localStorage.setItem(THEME_STORAGE_KEY, value);
                }}
              >
                <SelectTrigger className="h-8 w-40 text-xs"><SelectValue /></SelectTrigger>
                <SelectContent>
                  {Object.entries(EDITOR_THEMES).map(([key, label]) => (
                    <SelectItem key={key} value={key}>{label}</SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
          </DialogHeader>
          <div className="overflow-hidden rounded-md border">
            <Suspense fallback={
              <div className="flex h-[60vh] items-center justify-center">
                <Loader2 className="h-5 w-5 animate-spin text-muted-foreground" />
              </div>
            }>
              {editor && (
                <FileEditor
                  filename={editor.name}
                  value={editor.content}
                  theme={theme}
                  onChange={(next) => {
                    setEditor((previous) => (previous ? { ...previous, content: next } : previous));
                    setEditorDirty(true);
                  }}
                />
              )}
            </Suspense>
          </div>
          <DialogFooter>
            <Button variant="ghost" onClick={() => { setEditor(null); setEditorDirty(false); }}>
              取消
            </Button>
            <Button onClick={() => void saveEditor()} disabled={busy === "save"}>
              {busy === "save" && <Loader2 className="mr-1 h-4 w-4 animate-spin" />}保存
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 管理员删除前的确认：可选择不放入回收站 */}
      <Dialog open={Boolean(deleting)} onOpenChange={(open) => { if (!open) setDeleting(null); }}>
        <DialogContent className="max-w-md">
          <DialogHeader>
            <DialogTitle>
              {deleting && deleting.length === 1 ? `删除 ${deleting[0]}` : `删除 ${deleting?.length ?? 0} 项`}
            </DialogTitle>
            <DialogDescription>
              {deletePermanent
                ? "所选项目将被永久删除，无法恢复。"
                : "所选项目将移入回收站，7 天内可还原。"}
            </DialogDescription>
          </DialogHeader>
          <label className="flex items-start gap-2 text-sm">
            <input
              type="checkbox"
              className="mt-0.5"
              checked={deletePermanent}
              onChange={(event) => setDeletePermanent(event.target.checked)}
            />
            <span>
              不放入回收站，直接删除
              <span className="block text-xs text-muted-foreground">仅管理员可用。删除后无法恢复。</span>
            </span>
          </label>
          <DialogFooter>
            <Button variant="ghost" onClick={() => setDeleting(null)}>取消</Button>
            <Button
              variant="destructive"
              onClick={() => {
                const names = deleting || [];
                const permanent = deletePermanent;
                setDeleting(null);
                void performDelete(names, permanent);
              }}
            >
              {deletePermanent ? "永久删除" : "删除"}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <FileTrashDialog open={trashOpen} onOpenChange={setTrashOpen} call={call} onChanged={() => void load()} />
    </Card>
  );
}
