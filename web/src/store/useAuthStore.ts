import { create } from 'zustand';
import type { CurrentUser } from '../types/api';
import { login as apiLogin, logout as apiLogout, fetchMe } from '../api/auth';

// useAuthStore (web UI design — V4 CC-02 HttpOnly Cookie refactor).
//
// The auth token is NEVER stored in the frontend (no localStorage / no
// sessionStorage — that is an XSS one-shot per). It lives in an HttpOnly
// cookie set by the backend on login. This store only tracks UI-level state:
//   - isAuthenticated  — derived from a /api/v1/auth/me cookie probe
//   - currentUser      — { id, email, role } from /api/v1/auth/me
//   - edition          — deployment edition tag (drives feature-flag UI)
//
// All auth fetches go through src/api/auth.ts which sends credentials:'include'
// (cookie auto-attached) + X-CSRF-Token on mutations. Standalone:
// auth.ts falls back to a mock session when the backend is unreachable, so the
// UI is fully exercisable without a live server.

interface AuthState {
  edition: string;
  isAuthenticated: boolean;
  currentUser: CurrentUser | null;
  /** True until the initial cookie probe (bootstrap) resolves. */
  authChecked: boolean;

  /** Cookie probe on app start — populates session from /auth/me if valid. */
  probe: () => Promise<void>;
  /** Email + password login → backend Set-Cookie HttpOnly + CSRF. */
  login: (email: string, password: string) => Promise<void>;
  /** Logout → backend clears cookies. */
  logout: () => Promise<void>;
}

export const useAuthStore = create<AuthState>((set) => ({
  edition: 'ce',
  isAuthenticated: false,
  currentUser: null,
  authChecked: false,

  probe: async () => {
    try {
      const me = await fetchMe();
      if (me) {
        set({
          isAuthenticated: true,
          currentUser: { id: me.id, email: me.email, display_name: me.display_name, role: me.role ?? 'user' },
          edition: me.edition ?? 'ce',
          authChecked: true,
        });
      } else {
        set({ isAuthenticated: false, currentUser: null, authChecked: true });
      }
    } catch {
      set({ isAuthenticated: false, currentUser: null, authChecked: true });
    }
  },

  login: async (email, password) => {
    const user = await apiLogin(email, password);
    set({
      isAuthenticated: true,
      currentUser: { id: user.id, email: user.email, display_name: user.display_name, role: user.role ?? 'user' },
      edition: user.edition ?? 'ce',
    });
  },

  logout: async () => {
    await apiLogout();
    set({ isAuthenticated: false, currentUser: null });
  },
}));
