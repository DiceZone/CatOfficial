/**
 * 网页终端（照搬 BDC「骰子之间」的 NodeTerminal）
 *
 * 终端跑在 CatOfficial 所在的机器上，网页只是个窗口——关掉页面不影响它继续跑，回来接着看。
 * 可以同时开多个，标签页列的是这台机器上正在跑的会话。
 *
 * # 为什么是轮询而不是 WebSocket
 *
 * 面板常被放在反向代理、CDN 后面，WebSocket 在这中间出过太多问题（BDC 的经验）。
 * 输出走长轮询：请求挂着，有输出立刻回来，没有就等到超时再发一个。按键单独走一条，不必等读那条回来。
 */
import { useCallback, useEffect, useRef, useState } from "react";
import { useTranslation } from "react-i18next";
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { PageHeader } from '@/components/page-header';
import { ExposedWarning } from '@/components/exposed-warning';
import { useStyledDialog } from '@/components/styled-dialog';
import { apiClient, useToast } from '@/lib/bdc-compat';
import { Plus, RefreshCw, TerminalSquare, X } from "lucide-react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";

interface SessionInfo {
  id: string;
  label: string;
  shell: string;
  opened: string;
  alive: boolean;
  bytes: number;
}

export function decodeChunk(b64: string): Uint8Array {
  const binary = atob(b64);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) out[i] = binary.charCodeAt(i);
  return out;
}

export function encodeKeys(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let binary = "";
  bytes.forEach((b) => { binary += String.fromCharCode(b); });
  return btoa(binary);
}

export function TerminalPage() {
  const { t } = useTranslation();
  const dialog = useStyledDialog();
  const [tabs, setTabs] = useState<SessionInfo[]>([]);
  const [activeKey, setActiveKey] = useState("");
  const [exposed, setExposed] = useState(false);
  const toast = useToast();
  const [busy, setBusy] = useState("");
  const [adding, setAdding] = useState(false);
  const [newLabel, setNewLabel] = useState("");

  const holder = useRef<HTMLDivElement | null>(null);
  //: 标签条换行会把终端整体往下推，高度得跟着重算
  const strip = useRef<HTMLDivElement | null>(null);
  const term = useRef<Terminal | null>(null);
  const fit = useRef<FitAddon | null>(null);
  //: 读到哪儿了。放 ref 不放 state——每次轮询都变，进 state 会把组件刷爆。
  const cursor = useRef(0);
  //: 轮询循环靠它退出：切换标签、卸载时置空
  const reading = useRef("");

  const active = tabs.find((tab) => tab.id === activeKey) || null;
  //: 建终端那个 effect 只跑一次，闭包里拿不到后来的 active，用 ref 兜住
  const activeRef = useRef<SessionInfo | null>(null);
  activeRef.current = active;

  /** 读一遍这台机器上正在跑的会话。 */
  const rescan = useCallback(async () => {
    setBusy("scan");
    try {
      const resp = await apiClient.get<{ sessions: SessionInfo[]; exposed: boolean }>("/api/webui/terminal/sessions");
      setTabs(resp.sessions || []);
      setExposed(Boolean(resp.exposed));
    } catch (err: any) {
      toast.error(err?.body?.detail || "读取终端列表失败");
    } finally {
      setBusy("");
    }
  }, []);

  useEffect(() => { void rescan(); }, [rescan]);

  /** 建终端窗口。只建一次，切换标签时清屏复用。 */
  useEffect(() => {
    if (!holder.current || term.current) return;
    const instance = new Terminal({
      fontSize: 13,
      fontFamily: "'SF Mono','Fira Code',Menlo,Consolas,'Microsoft YaHei',monospace",
      cursorBlink: true,
      scrollback: 5000,
      theme: { background: "#0f0f12", foreground: "#e4e4e7" },
    });
    const fitAddon = new FitAddon();
    instance.loadAddon(fitAddon);
    instance.open(holder.current);
    fitAddon.fit();
    term.current = instance;
    fit.current = fitAddon;

    // 撑到视口底部，重排，然后把新尺寸告诉后端。
    //
    // 高度量出来而不是写死：上面加一行标题、标签条换一次行，写死的像素值
    // 就不对了。只在本地 fit 也不够——shell 那边还按旧的行列数排版，htop
    // 这类全屏程序会被裁掉下半截，而那正是开终端要跑的东西。
    let timer = 0;
    const relayout = () => {
      window.clearTimeout(timer);
      timer = window.setTimeout(() => {
        const box = holder.current;
        if (!box) return;
        const top = box.getBoundingClientRect().top;
        //: 底部留一点，别让终端正好顶到窗口边缘
        const wanted = Math.max(320, Math.round(window.innerHeight - top - 28));
        if (Math.abs(box.clientHeight - wanted) > 2) {
          box.style.height = `${wanted}px`;
        }
        try {
          fitAddon.fit();
        } catch {
          return; // 容器还没布好局，下次再说
        }
        const dims = fitAddon.proposeDimensions();
        const current = activeRef.current;
        if (dims && current) {
          void apiClient.post("/api/webui/terminal/resize", { session_id: current.id, cols: dims.cols, rows: dims.rows })
            .catch(() => { /* 调不动不影响用 */ });
        }
      }, 120);
    };

    // 观察标签条而不是终端本身：终端的高度是这里改的，观察它会自己触发自己。
    // 标签条换行（加标签、侧边栏收起）会把终端往下推，正是要接住的那一类变化。
    const observer = new ResizeObserver(relayout);
    if (strip.current) observer.observe(strip.current);
    window.addEventListener("resize", relayout);
    relayout();
    return () => {
      window.clearTimeout(timer);
      observer.disconnect();
      window.removeEventListener("resize", relayout);
      instance.dispose();
      term.current = null;
    };
  }, []);

  /** 按键直接送到后端，不等读那条请求回来——否则打字会一顿一顿的。 */
  useEffect(() => {
    const instance = term.current;
    if (!instance || !active) return;
    const sub = instance.onData((data) => {
      void apiClient.post("/api/webui/terminal/input", { session_id: active.id, data: encodeKeys(data) })
        .catch(() => { /* 丢一个键不值得打断，下次输入自会重试 */ });
    });
    return () => sub.dispose();
  }, [active]);

  /** 输出轮询。每次带上游标，后端给后面的内容；没有就挂着等。 */
  useEffect(() => {
    if (!active) return;
    const key = active.id;
    reading.current = key;
    let stopped = false;

    void (async () => {
      while (!stopped && reading.current === key) {
        try {
          const resp = await apiClient.post<{ data: string; cursor: number; alive: boolean }>(
            "/api/webui/terminal/read", { session_id: active.id, cursor: cursor.current },
          );
          if (stopped || reading.current !== key) break;
          if (resp.data) term.current?.write(decodeChunk(resp.data));
          cursor.current = resp.cursor ?? cursor.current;
          if (!resp.alive) {
            term.current?.writeln(`\r\n\x1b[33m[${t("terminal.ended")}]\x1b[0m`);
            setTabs((list) => list.map((tab) => (tab.id === key ? { ...tab, alive: false } : tab)));
            break;
          }
        } catch {
          // 网络抖一下不该把终端断掉，缓一秒接着轮
          await new Promise((done) => setTimeout(done, 1000));
        }
      }
    })();

    return () => { stopped = true; };
  }, [active?.id]);

  function attach(tab: SessionInfo) {
    reading.current = "";
    cursor.current = 0;
    term.current?.reset();
    setActiveKey(tab.id);
    // 接上之后把窗口大小同步过去，否则 shell 还按上一个尺寸排版
    const dims = fit.current?.proposeDimensions();
    if (dims) {
      void apiClient.post("/api/webui/terminal/resize", { session_id: tab.id, cols: dims.cols, rows: dims.rows })
        .catch(() => { /* 调不动不影响用 */ });
    }
    term.current?.focus();
  }

  async function createTab() {
    setBusy("open");
    try {
      const dims = fit.current?.proposeDimensions();
      const created = await apiClient.post<{ id: string; shell: string; opened: string }>(
        "/api/webui/terminal",
        { label: newLabel.trim(), cols: dims?.cols ?? 100, rows: dims?.rows ?? 30 },
      );
      const tab: SessionInfo = {
        id: created.id, label: newLabel.trim(), shell: created.shell,
        opened: created.opened || new Date().toISOString(), alive: true, bytes: 0,
      };
      setTabs((list) => [...list, tab]);
      setNewLabel("");
      setAdding(false);
      attach(tab);
    } catch (err: any) {
      toast.error(err?.body?.detail || t("terminal.open_failed"));
    } finally {
      setBusy("");
    }
  }

  async function closeTab(tab: SessionInfo) {
    const confirmed = await dialog.confirm({
      title: t("terminal.close_title", { name: tab.label || tab.id.slice(0, 6) }),
      description: t("terminal.close_desc"),
      confirmText: t("terminal.close"),
    });
    if (!confirmed) return;
    setBusy(`close-${tab.id}`);
    try {
      await apiClient.post("/api/webui/terminal/close", { session_id: tab.id });
      if (activeKey === tab.id) {
        reading.current = "";
        setActiveKey("");
        term.current?.reset();
      }
      setTabs((list) => list.filter((item) => item.id !== tab.id));
    } catch (err: any) {
      toast.error(err?.body?.detail || t("terminal.close_failed"));
    } finally {
      setBusy("");
    }
  }

  return (
    <>
      <PageHeader title={t("terminal.title")} description={t("terminal.description")} />
      {exposed && <ExposedWarning />}
      <div className="space-y-3">
        {/* 标签栏 */}
        <div ref={strip} className="flex flex-wrap items-center gap-2">
          {tabs.map((tab) => (
            <div
              key={tab.id}
              className={
                "flex items-center gap-2 rounded-md border px-3 py-1.5 text-sm " +
                (activeKey === tab.id ? "border-primary bg-primary/10" : "hover:bg-muted")
              }
            >
              <button type="button" onClick={() => attach(tab)} className="flex items-center gap-2">
                <TerminalSquare className="h-3.5 w-3.5 text-muted-foreground" />
                <span className="font-medium">{tab.label || tab.id.slice(0, 6)}</span>
              </button>
              {!tab.alive && <Badge variant="secondary">{t("terminal.ended")}</Badge>}
              <button
                type="button"
                title={t("terminal.close")}
                disabled={busy === `close-${tab.id}`}
                onClick={() => void closeTab(tab)}
                className="text-muted-foreground hover:text-destructive"
              >
                <X className="h-3.5 w-3.5" />
              </button>
            </div>
          ))}

          <Button size="sm" variant="outline" onClick={() => setAdding((v) => !v)}>
            <Plus className="mr-1 h-4 w-4" />{t("terminal.new")}
          </Button>
          <Button size="sm" variant="ghost" title={t("terminal.rescan")} disabled={busy === "scan"} onClick={() => void rescan()}>
            <RefreshCw className={"h-4 w-4 " + (busy === "scan" ? "animate-spin" : "")} />
          </Button>
        </div>

        <Dialog open={adding} onOpenChange={setAdding}>
          <DialogContent className="max-w-md">
            <DialogHeader>
              <DialogTitle>{t("terminal.new")}</DialogTitle>
            </DialogHeader>
            <div className="space-y-1.5">
              <Label htmlFor="terminal-label">{t("terminal.label")}</Label>
              <Input
                id="terminal-label"
                value={newLabel}
                placeholder={t("terminal.label_placeholder")}
                onChange={(e) => setNewLabel(e.target.value)}
                onKeyDown={(e) => { if (e.key === "Enter") void createTab(); }}
              />
            </div>
            <DialogFooter>
              <Button variant="outline" onClick={() => setAdding(false)}>{t("common.cancel")}</Button>
              <Button disabled={busy === "open"} onClick={() => void createTab()}>
                {busy === "open" ? t("terminal.opening") : t("terminal.open")}
              </Button>
            </DialogFooter>
          </DialogContent>
        </Dialog>

        <Card className="border-slate-300 dark:border-slate-900">
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <TerminalSquare className="h-5 w-5" />{t("terminal.card_title")}
            </CardTitle>
            <CardDescription>
              {active
                ? `${active.label || active.id.slice(0, 6)} · ${active.shell}`
                : (tabs.length ? t("terminal.pick") : t("terminal.empty"))}
            </CardDescription>
          </CardHeader>

          {/* 终端区套一块灰色面板：黑底不直接贴着卡片，一眼能看出「这一块是另一个世界」。 */}
          <CardContent>
            <div className="rounded-lg border border-slate-300 bg-slate-100 p-3 dark:border-slate-900 dark:bg-slate-800">
              {/* 高度在运行时量出来，不写死像素：写死的话上面加一行标题、标签条
                  换一次行，htop 这类全屏程序就被裁掉下半截。 */}
              <div ref={holder} className="min-h-[320px] w-full overflow-hidden rounded-md bg-[#0f0f12] p-2" />
            </div>
          </CardContent>
        </Card>
      </div>
    </>
  );
}
