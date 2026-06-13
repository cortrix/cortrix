import { API_BASE } from '../utils/constants';
import { getCsrfToken, CSRF_HEADER } from '../utils/csrf';
import { recordApiLatency, statusBucket } from '../telemetry/metrics';

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

const SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);

async function request<T>(path: string, options?: RequestInit): Promise<T> {
  const url = `${API_BASE}${path}`;
  const method = (options?.method ?? 'GET').toUpperCase();
  // Auth via HttpOnly cookie (P02a § 4.5): always send credentials so the
  // browser attaches `cortrix-auth`. Mutating requests echo the readable CSRF
  // cookie back in the X-CSRF-Token header (P02a § 4.6 double-cookie pattern).
  const csrfHeader: Record<string, string> = SAFE_METHODS.has(method)
    ? {}
    : { [CSRF_HEADER]: getCsrfToken() };
  // § 23-bis.1 cortrix_webui_api_latency_seconds — client-perceived latency.
  const start = typeof performance !== 'undefined' ? performance.now() : 0;
  const elapsed = () => (start ? (performance.now() - start) / 1000 : 0);
  let res: Response;
  try {
    res = await fetch(url, {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json', ...csrfHeader, ...options?.headers },
      ...options,
    });
  } catch (networkErr) {
    if (start) recordApiLatency(path, '5xx', method, elapsed());
    throw networkErr;
  }
  if (start) recordApiLatency(path, statusBucket(res.status), method, elapsed());
  if (!res.ok) {
    const body = await res.text().catch(() => '');
    throw new ApiError(res.status, body || res.statusText);
  }
  if (res.status === 204 || res.headers.get('content-length') === '0') {
    return undefined as T;
  }
  return res.json();
}

export function get<T>(path: string): Promise<T> {
  return request<T>(path);
}

export function post<T>(path: string, body?: unknown): Promise<T> {
  return request<T>(path, {
    method: 'POST',
    body: body ? JSON.stringify(body) : undefined,
  });
}

export function patch<T>(path: string, body?: unknown): Promise<T> {
  return request<T>(path, {
    method: 'PATCH',
    body: body ? JSON.stringify(body) : undefined,
  });
}

export function put<T>(path: string, body?: unknown): Promise<T> {
  return request<T>(path, {
    method: 'PUT',
    body: body ? JSON.stringify(body) : undefined,
  });
}

export function del<T>(path: string): Promise<T> {
  return request<T>(path, { method: 'DELETE' });
}

export async function uploadFile(
  path: string,
  file: File,
  onProgress?: (pct: number) => void,
): Promise<Response> {
  const formData = new FormData();
  formData.append('file', file);

  // Use XMLHttpRequest for progress tracking
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', `${API_BASE}${path}`);

    xhr.upload.addEventListener('progress', (e) => {
      if (e.lengthComputable && onProgress) {
        onProgress(Math.round((e.loaded / e.total) * 100));
      }
    });

    xhr.addEventListener('load', () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(new Response(xhr.responseText, { status: xhr.status }));
      } else {
        reject(new ApiError(xhr.status, xhr.responseText));
      }
    });

    xhr.addEventListener('error', () => reject(new Error('Network error')));
    xhr.send(formData);
  });
}
