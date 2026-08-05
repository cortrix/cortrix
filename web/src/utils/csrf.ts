// CSRF token helper (web UI design § 4.6 — double-cookie + custom header).
//
// The backend issues two cookies on login: `cortrix-auth` (HttpOnly — proof of
// identity, NOT readable by JS) and `cortrix-csrf` (non-HttpOnly — readable by
// JS, echoed back in the X-CSRF-Token header on mutating requests). The server
// validates `cookies['cortrix-csrf'] == headers['X-CSRF-Token']` for
// POST/PUT/PATCH/DELETE. Safe methods (GET/HEAD/OPTIONS) are not checked.

export const CSRF_COOKIE = 'cortrix-csrf';
export const CSRF_HEADER = 'X-CSRF-Token';

/** Read the non-HttpOnly CSRF cookie. Returns '' when absent (dev / pre-login). */
export function getCsrfToken(): string {
  if (typeof document === 'undefined') return '';
  return (
    document.cookie
      .split('; ')
      .find((row) => row.startsWith(`${CSRF_COOKIE}=`))
      ?.split('=')[1] ?? ''
  );
}
