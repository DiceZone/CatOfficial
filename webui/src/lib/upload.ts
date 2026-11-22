/**
 * 带进度的上传。
 *
 * # 为什么不用 fetch
 *
 * fetch 到今天仍然读不到上传进度——它只在下载方向给 ReadableStream。传一个
 * 一百多兆的 data 包时，界面上除了一个不动的转圈什么都没有，人分不清是在传
 * 还是卡死了，然后就去点第二次。XMLHttpRequest 老,但 upload.onprogress 是
 * 目前唯一能拿到已传字节数的办法。
 */

export interface UploadProgress {
  /** 已传字节 */
  loaded: number;
  /** 总字节。个别浏览器在某些情况下给不出来，那时是 0 */
  total: number;
  /** 0..100，总字节未知时停在 0 */
  percent: number;
}

export interface UploadOptions {
  url: string;
  body: Blob | File;
  headers?: Record<string, string>;
  onProgress?: (progress: UploadProgress) => void;
  /** 让调用方能中途取消 */
  signal?: AbortSignal;
}

export interface UploadResult {
  status: number;
  /** 响应体按 JSON 解析；解析不了就是 null，原文在 text 里 */
  json: any;
  text: string;
}

export function uploadWithProgress(options: UploadOptions): Promise<UploadResult> {
  const { url, body, headers = {}, onProgress, signal } = options;

  return new Promise<UploadResult>((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", url, true);
    for (const [name, value] of Object.entries(headers)) {
      request.setRequestHeader(name, value);
    }

    request.upload.onprogress = (event) => {
      if (!onProgress) return;
      const total = event.lengthComputable ? event.total : 0;
      onProgress({
        loaded: event.loaded,
        total,
        percent: total ? Math.min(100, Math.round((event.loaded / total) * 100)) : 0,
      });
    };

    // 传完了但服务端还在解包——这段时间进度条该停在 100% 而不是回到未知
    request.upload.onload = () => onProgress?.({
      loaded: body.size, total: body.size, percent: 100,
    });

    request.onload = () => {
      let json: any = null;
      try {
        json = JSON.parse(request.responseText);
      } catch {
        json = null;
      }
      resolve({ status: request.status, json, text: request.responseText });
    };
    request.onerror = () => reject(new Error("网络中断，上传没有完成"));
    request.ontimeout = () => reject(new Error("上传超时"));
    request.onabort = () => reject(new Error("上传已取消"));

    if (signal) {
      if (signal.aborted) {
        request.abort();
        return;
      }
      signal.addEventListener("abort", () => request.abort(), { once: true });
    }

    request.send(body);
  });
}
