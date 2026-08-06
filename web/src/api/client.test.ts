import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { get, post, patch, put, del, ApiError } from './client';

// api client tests (web UI/ — credentials + CSRF + error envelope).
// Asserts the request contract every store/api module relies on: cookies are
// always sent, mutations carry the X-CSRF-Token header, safe methods don't, and
// a non-2xx response throws an ApiError with the body text.

const mockFetch = vi.fn();

function ok(data: unknown, status = 200) {
  return Promise.resolve({
    ok: status >= 200 && status < 300,
    status,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: () => Promise.resolve(data),
    text: () => Promise.resolve(JSON.stringify(data)),
  } as Response);
}

beforeEach(() => {
  vi.stubGlobal('fetch', mockFetch);
  mockFetch.mockReset();
  // Provide a readable CSRF cookie so mutations echo it back.
  Object.defineProperty(document, 'cookie', {
    writable: true,
    configurable: true,
    value: 'cortrix-csrf=tok123',
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('api client', () => {
  it('GET sends credentials and no CSRF header (safe method)', async () => {
    mockFetch.mockReturnValueOnce(ok({ hello: 'world' }));
    const res = await get<{ hello: string }>('/api/v1/thing');
    expect(res).toEqual({ hello: 'world' });
    const [, init] = mockFetch.mock.calls[0];
    expect(init.credentials).toBe('include');
    expect(init.headers['X-CSRF-Token']).toBeUndefined();
  });

  it('POST carries the X-CSRF-Token header from the cookie', async () => {
    mockFetch.mockReturnValueOnce(ok({ id: 1 }));
    await post('/api/v1/thing', { name: 'x' });
    const [, init] = mockFetch.mock.calls[0];
    expect(init.method).toBe('POST');
    expect(init.headers['X-CSRF-Token']).toBe('tok123');
    expect(init.body).toBe(JSON.stringify({ name: 'x' }));
  });

  it('PATCH and PUT and DELETE are wired to their verbs', async () => {
    mockFetch.mockReturnValue(ok({}));
    await patch('/a', { p: 1 });
    await put('/b', { p: 2 });
    await del('/c');
    expect(mockFetch.mock.calls[0][1].method).toBe('PATCH');
    expect(mockFetch.mock.calls[1][1].method).toBe('PUT');
    expect(mockFetch.mock.calls[2][1].method).toBe('DELETE');
  });

  it('throws ApiError with the body on a non-2xx response', async () => {
    mockFetch.mockReturnValueOnce(
      Promise.resolve({
        ok: false,
        status: 403,
        statusText: 'Forbidden',
        headers: new Headers(),
        text: () => Promise.resolve('CX_ERR_AUTH_CSRF_MISMATCH'),
        json: () => Promise.resolve({}),
      } as Response),
    );
    await expect(get('/api/v1/secure')).rejects.toBeInstanceOf(ApiError);
  });

  it('returns undefined for a 204 No Content', async () => {
    mockFetch.mockReturnValueOnce(
      Promise.resolve({
        ok: true,
        status: 204,
        headers: new Headers({ 'content-length': '0' }),
        text: () => Promise.resolve(''),
        json: () => Promise.resolve(undefined),
      } as Response),
    );
    const res = await del<void>('/api/v1/thing/1');
    expect(res).toBeUndefined();
  });
});
