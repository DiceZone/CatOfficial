/**
 * 编辑器的主题清单与语法判定 —— 刻意不碰 CodeMirror
 *
 * 文件管理器需要在「还没打开编辑器」的时候就渲染主题下拉框、显示语法名。
 * 如果这些常量放在 FileEditor.tsx 里，从那边静态 import 一个名字就会把整包
 * CodeMirror 拖进主 bundle，React.lazy 那层动态引入等于白写——实测主包会
 * 从几百 KB 涨到 1.6 MB。
 *
 * 所以这里只有纯数据，真正的主题扩展在 FileEditor.tsx 里按同样的键映射。
 */

/** 主题键 → 给人看的名字。值必须和 FileEditor 里的映射保持一致。 */
export const EDITOR_THEMES = {
  "github-light": "GitHub 浅色",
  "github-dark": "GitHub 深色",
  "vscode-dark": "VS Code 深色",
  dracula: "Dracula",
  "tokyo-night": "Tokyo Night",
} as const;

export type EditorThemeKey = keyof typeof EDITOR_THEMES;

export const DEFAULT_THEME: EditorThemeKey = "github-dark";

/** 文件的语法名，显示在编辑器标题下面。认不出就说纯文本，不猜。 */
export function languageLabel(filename: string): string {
  const lower = filename.toLowerCase();
  if (lower.endsWith(".json")) return "JSON";
  if (lower.endsWith(".yaml") || lower.endsWith(".yml")) return "YAML";
  if (lower.endsWith(".ts")) return "TypeScript";
  if (lower.endsWith(".js") || lower.endsWith(".mjs")) return "JavaScript";
  if (lower.endsWith(".lua")) return "Lua";
  if (lower.endsWith(".sh")) return "Shell";
  if (lower.endsWith(".ini") || lower.endsWith(".conf") || lower.endsWith(".env")) return "配置";
  return "纯文本";
}
