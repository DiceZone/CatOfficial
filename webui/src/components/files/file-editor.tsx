/**
 * FileEditor — 文件管理器里的代码编辑器
 *
 * 用 CodeMirror 6 而不是 Monaco：Monaco 光核心就好几 MB，还要额外配 web worker
 * 才能跑语法服务，而这里只是改改 dice.yaml 和牌堆 JSON，用不上那套东西。
 * CodeMirror 体积小一个数量级，行号、折叠、搜索、多光标都是自带的。
 *
 * 整个模块由外面用 React.lazy 动态引入：不打开编辑器的人不该为它付出首屏体积。
 */

import { useMemo } from "react";
import type { Extension } from "@codemirror/state";
import { DEFAULT_THEME, type EditorThemeKey } from "./editor-themes";
import CodeMirror, { EditorView } from "@uiw/react-codemirror";
import { json } from "@codemirror/lang-json";
import { yaml } from "@codemirror/lang-yaml";
import { javascript } from "@codemirror/lang-javascript";
import { StreamLanguage } from "@codemirror/language";
import { lua } from "@codemirror/legacy-modes/mode/lua";
import { shell } from "@codemirror/legacy-modes/mode/shell";
import { properties } from "@codemirror/legacy-modes/mode/properties";
import { githubLight, githubDark } from "@uiw/codemirror-theme-github";
import { vscodeDark } from "@uiw/codemirror-theme-vscode";
import { dracula } from "@uiw/codemirror-theme-dracula";
import { tokyoNight } from "@uiw/codemirror-theme-tokyo-night";

/** 主题键 → CodeMirror 扩展与明暗。键必须和 editor-themes.ts 里那份清单对齐。 */
const THEME_EXTENSIONS: Record<EditorThemeKey, { extension: Extension; dark: boolean }> = {
  "github-light": { extension: githubLight, dark: false },
  "github-dark": { extension: githubDark, dark: true },
  "vscode-dark": { extension: vscodeDark, dark: true },
  dracula: { extension: dracula, dark: true },
  "tokyo-night": { extension: tokyoNight, dark: true },
};

/**
 * 编辑器里的滚动条，按编辑器主题的明暗配色。
 *
 * 全站那条滚动条是 6px、前景色 30% 透明，跟着的是网站的明暗；而编辑器有自己的
 * 主题（默认 GitHub 深色），两边对不上——深色编辑器上那条滑块几乎看不见，6px 也
 * 很难点中。做法同日志面板（InstanceContainers）：
 *
 * - scrollbar-color / scrollbar-width：Chrome、Edge、Firefox 认，恢复系统标准宽度，
 *   滑块换成看得见的灰。设了它们，Chrome 就不再套全站的 ::-webkit-scrollbar。
 * - ::-webkit-scrollbar：给还不认 scrollbar-color 的 Safari。14px 宽，滑块四周留
 *   3px 透明边，看上去 8px，能点中的是整整 14px。
 */
function scrollbarFor(dark: boolean): Extension {
  const thumb = dark ? "#71717a" : "#a1a1aa";
  const thumbHover = dark ? "#a1a1aa" : "#71717a";
  return EditorView.theme({
    ".cm-scroller": {
      scrollbarWidth: "auto",
      scrollbarColor: `${thumb} transparent`,
    },
    ".cm-scroller::-webkit-scrollbar": { width: "14px", height: "14px" },
    ".cm-scroller::-webkit-scrollbar-track, .cm-scroller::-webkit-scrollbar-corner": {
      backgroundColor: "transparent",
    },
    ".cm-scroller::-webkit-scrollbar-thumb": {
      backgroundColor: thumb,
      backgroundClip: "content-box",
      border: "3px solid transparent",
      borderRadius: "7px",
    },
    ".cm-scroller::-webkit-scrollbar-thumb:hover": { backgroundColor: thumbHover },
  });
}

/** 按扩展名挑语法。认不出来就当纯文本——高亮错了比没高亮更碍事。 */
function languageOf(filename: string) {
  const lower = filename.toLowerCase();
  if (lower.endsWith(".json")) return [json()];
  if (lower.endsWith(".yaml") || lower.endsWith(".yml")) return [yaml()];
  if (lower.endsWith(".js") || lower.endsWith(".mjs") || lower.endsWith(".ts")) {
    return [javascript({ typescript: lower.endsWith(".ts") })];
  }
  if (lower.endsWith(".lua")) return [StreamLanguage.define(lua)];
  if (lower.endsWith(".sh")) return [StreamLanguage.define(shell)];
  if (lower.endsWith(".ini") || lower.endsWith(".conf") || lower.endsWith(".env")) {
    return [StreamLanguage.define(properties)];
  }
  return [];
}

export function FileEditor({
  filename,
  value,
  theme,
  onChange,
}: {
  filename: string;
  value: string;
  theme: EditorThemeKey;
  onChange: (next: string) => void;
}) {
  const resolved = THEME_EXTENSIONS[theme] ?? THEME_EXTENSIONS[DEFAULT_THEME];
  const extensions = useMemo(
    () => [...languageOf(filename), scrollbarFor(resolved.dark)],
    [filename, resolved.dark],
  );

  return (
    <CodeMirror
      value={value}
      height="60vh"
      theme={resolved.extension}
      extensions={extensions}
      onChange={onChange}
      basicSetup={{
        lineNumbers: true,
        foldGutter: true,
        highlightActiveLine: true,
        highlightActiveLineGutter: true,
        bracketMatching: true,
        closeBrackets: true,
        autocompletion: true,
        highlightSelectionMatches: true,
        searchKeymap: true,
        // 骰子的配置文件里制表符和空格混用会出事，统一成空格
        tabSize: 2,
      }}
    />
  );
}
