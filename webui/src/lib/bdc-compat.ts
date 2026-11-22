// 照搬自 BDC（骰子之间）的文件管理与终端组件用到的几个 shared-ui 接口，接到 CatOfficial 自己的实现上。
// 保持同样的调用方式与错误形状（err.body.detail），组件本身就不用大改，以后从 BDC 同步改动也容易。

import { toast } from 'sonner';
import { ApiError, request } from '@/lib/api';

/** BDC 的 apiClient：路径以 /api/webui/ 开头；出错时抛 {status, body: {detail}}。 */
async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const relative = path.replace(/^\/?api\/webui\//, '');
  try {
    return await request<T>(method, relative, body);
  } catch (error) {
    if (error instanceof ApiError) throw { status: error.status, body: { detail: error.message }, message: error.message };
    throw { status: 0, body: { detail: String(error) }, message: String(error) };
  }
}

export const apiClient = {
  get: <T>(path: string) => call<T>('GET', path),
  post: <T>(path: string, body?: unknown) => call<T>('POST', path, body ?? {}),
  delete: <T>(path: string) => call<T>('DELETE', path),
};

/** BDC 的 useToast：success / error / warning / info。 */
export function useToast() {
  return {
    success: (message: string) => toast.success(message),
    error: (message: string) => toast.error(message),
    warning: (message: string) => toast.warning(message),
    info: (message: string) => toast(message),
  };
}

/** 时间显示。BDC 固定按北京时间；CatOfficial 装在用户自己的机器上，按浏览器所在时区显示。 */
export function formatDateTime(value: string | number | Date | null | undefined, options: Intl.DateTimeFormatOptions = {}): string {
  if (value === null || value === undefined || value === '') return '—';
  const date = value instanceof Date ? value : new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString(undefined, { hour12: false, ...options });
}
