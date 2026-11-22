// 提示、确认、输入三种弹窗（照搬 BDC shared-ui 的 StyledDialogProvider，文件管理与终端页用）。
import * as React from "react";
import { useTranslation } from "react-i18next";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";

interface BaseOptions {
  title: string;
  /** 一段文字（换行照排），或自己排好的内容（分行、强调） */
  description?: React.ReactNode;
  confirmText?: string;
}

interface ConfirmOptions extends BaseOptions {
  cancelText?: string;
  destructive?: boolean;
}

interface PromptOptions extends ConfirmOptions {
  defaultValue?: string;
  placeholder?: string;
}

interface DialogApi {
  alert: (options: BaseOptions) => Promise<void>;
  confirm: (options: ConfirmOptions) => Promise<boolean>;
  prompt: (options: PromptOptions) => Promise<string | null>;
}

type PendingDialog =
  | ({ kind: "alert"; resolve: () => void } & BaseOptions)
  | ({ kind: "confirm"; resolve: (value: boolean) => void } & ConfirmOptions)
  | ({ kind: "prompt"; resolve: (value: string | null) => void } & PromptOptions);

const StyledDialogContext = React.createContext<DialogApi | null>(null);

export function StyledDialogProvider({ children }: { children: React.ReactNode }) {
  const { t } = useTranslation();
  const [pending, setPending] = React.useState<PendingDialog | null>(null);
  const [inputValue, setInputValue] = React.useState("");

  const api = React.useMemo<DialogApi>(() => ({
    alert: (options) => new Promise<void>((resolve) => {
      setPending({ kind: "alert", ...options, resolve });
    }),
    confirm: (options) => new Promise<boolean>((resolve) => {
      setPending({ kind: "confirm", ...options, resolve });
    }),
    prompt: (options) => new Promise<string | null>((resolve) => {
      setInputValue(options.defaultValue ?? "");
      setPending({ kind: "prompt", ...options, resolve });
    }),
  }), []);

  const cancel = React.useCallback(() => {
    if (!pending) return;
    if (pending.kind === "alert") pending.resolve();
    else if (pending.kind === "confirm") pending.resolve(false);
    else pending.resolve(null);
    setPending(null);
  }, [pending]);

  const accept = React.useCallback(() => {
    if (!pending) return;
    if (pending.kind === "alert") pending.resolve();
    else if (pending.kind === "confirm") pending.resolve(true);
    else pending.resolve(inputValue);
    setPending(null);
  }, [inputValue, pending]);

  return (
    <StyledDialogContext.Provider value={api}>
      {children}
      <Dialog open={pending !== null} onOpenChange={(open) => { if (!open) cancel(); }}>
        <DialogContent className="sm:max-w-md">
          <DialogHeader>
            <DialogTitle>{pending?.title}</DialogTitle>
            {pending?.description && (typeof pending.description === "string" ? (
              <DialogDescription className="whitespace-pre-line">{pending.description}</DialogDescription>
            ) : (
              // 排好的内容里会有列表、分段，不能套在 <p> 里
              <DialogDescription asChild>
                <div className="space-y-2">{pending.description}</div>
              </DialogDescription>
            ))}
          </DialogHeader>
          {pending?.kind === "prompt" && (
            <Input
              autoFocus
              value={inputValue}
              placeholder={pending.placeholder}
              onChange={(event) => setInputValue(event.target.value)}
              onKeyDown={(event) => { if (event.key === "Enter") accept(); }}
            />
          )}
          <DialogFooter className="gap-2">
            {pending?.kind !== "alert" && (
              <Button variant="outline" onClick={cancel}>{pending?.cancelText || t("common.cancel")}</Button>
            )}
            <Button
              variant={pending?.kind !== "alert" && pending?.destructive ? "destructive" : "default"}
              onClick={accept}
            >
              {pending?.confirmText || t("common.confirm")}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </StyledDialogContext.Provider>
  );
}

export function useStyledDialog(): DialogApi {
  const context = React.useContext(StyledDialogContext);
  if (!context) {
    throw new Error("useStyledDialog 必须在 StyledDialogProvider 内使用");
  }
  return context;
}
