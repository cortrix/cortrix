import { lazy, Suspense } from 'react';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { Layout } from './components/Layout/Layout';
import { PrivateRoute } from './pages/PrivateRoute';
import { BootstrapPage } from './pages/BootstrapPage';
import { LoginPage } from './pages/LoginPage';
import { NamespacesPage } from './components/Namespace/NamespacesPage';
import { LoadingSpinner } from './components/Common/LoadingSpinner';

// Route registry (web UI design — react-router-dom). Migration note (R3/S5):
// page selection moved off the old `activePage` zustand field onto the URL. The
// app shell (Layout) is itself guarded by PrivateRoute, so every authenticated
// page (all R1/R2 pages + admin pages) sits behind the auth cookie probe; the
// standalone setup/sign-in routes (/bootstrap, /login) stay public.
//
// Perf (web UI/ — S10 code-split): the landing route (Namespaces)
// and the public auth shell (Bootstrap / Login) stay eager so first paint is
// immediate. Every other page is React.lazy-imported so it ships in its own
// route chunk fetched on navigation — this pulls Chat (react-markdown),
// Settings, the admin tables, the Ent placeholders and the Health dashboard out
// of the initial bundle (web UI). A <Suspense> with the shared spinner
// covers the in-flight chunk fetch.
//
// Path mapping (keeps every R1/R2 page reachable):
//   /            -> Namespaces (eager landing)
//   /upload      -> Upload          /search   -> Search      /agent -> Agent
//   /memory      -> Memory          /settings -> Settings
//   /admin/users -> UsersPage (admin)
//   /admin/operation-log -> OperationLogPage (admin)
//
// Ent placeholder routes (/ent/text-to-sql, /ent/audit-log, /ent/multi-tenant,
//) are registered in V1.0 already (R4/S6) so bookmarks survive the V1.5
// swap; each page is a feature-flag gate (EntPages) rendering the
// marketing PlaceholderPage until the backend reports the feature enabled. The
// Health dashboard (/health, readiness /live + /ready) is also a guarded route
// (R4/S7).
//   /ent/text-to-sql  -> TextToSqlPage   /ent/audit-log -> AuditLogPage
//   /ent/multi-tenant -> MultiTenantPage

// Lazy route chunks (named exports → resolve to a default for React.lazy).
const UploadPage = lazy(() =>
  import('./components/Upload/UploadPage').then((m) => ({ default: m.UploadPage })),
);
const SearchPage = lazy(() =>
  import('./components/Search/SearchPage').then((m) => ({ default: m.SearchPage })),
);
const ChatPage = lazy(() =>
  import('./components/Chat/ChatPage').then((m) => ({ default: m.ChatPage })),
);
const MemoryPage = lazy(() =>
  import('./components/Memory/MemoryPage').then((m) => ({ default: m.MemoryPage })),
);
const SettingsPage = lazy(() =>
  import('./components/Settings/SettingsPage').then((m) => ({ default: m.SettingsPage })),
);
const HealthPage = lazy(() =>
  import('./pages/HealthPage').then((m) => ({ default: m.HealthPage })),
);
const UsersPage = lazy(() =>
  import('./pages/admin/UsersPage').then((m) => ({ default: m.UsersPage })),
);
const OperationLogPage = lazy(() =>
  import('./pages/admin/OperationLogPage').then((m) => ({ default: m.OperationLogPage })),
);
const TextToSqlPage = lazy(() =>
  import('./components/EntPlaceholder/EntPages').then((m) => ({ default: m.TextToSqlPage })),
);
const AuditLogPage = lazy(() =>
  import('./components/EntPlaceholder/EntPages').then((m) => ({ default: m.AuditLogPage })),
);
const MultiTenantPage = lazy(() =>
  import('./components/EntPlaceholder/EntPages').then((m) => ({ default: m.MultiTenantPage })),
);

function RouteFallback() {
  return (
    <div className="grid min-h-[40vh] place-items-center" data-testid="route-loading">
      <LoadingSpinner />
    </div>
  );
}

export function AppRoutes() {
  return (
    <BrowserRouter>
      <Suspense fallback={<RouteFallback />}>
        <Routes>
          {/* Public setup / auth routes */}
          <Route path="/bootstrap" element={<BootstrapPage />} />
          <Route path="/login" element={<LoginPage />} />

          {/* Authenticated app shell */}
          <Route
            element={
              <PrivateRoute>
                <Layout />
              </PrivateRoute>
            }
          >
            <Route index element={<NamespacesPage />} />
            <Route path="namespaces" element={<NamespacesPage />} />
            <Route path="upload" element={<UploadPage />} />
            <Route path="search" element={<SearchPage />} />
            <Route path="agent" element={<ChatPage />} />
            {/* legacy bookmark redirect: the surface moved to /agent */}
            <Route path="chat" element={<Navigate to="/agent" replace />} />
            <Route path="memory" element={<MemoryPage />} />
            <Route path="settings" element={<SettingsPage />} />
            <Route path="health" element={<HealthPage />} />

            {/* Ent placeholders (web UI) — feature-flag gated pages,
                registered in V1.0 so V1.5 bookmarks survive the swap. */}
            <Route path="ent/text-to-sql" element={<TextToSqlPage />} />
            <Route path="ent/audit-log" element={<AuditLogPage />} />
            <Route path="ent/multi-tenant" element={<MultiTenantPage />} />

            {/* Admin (Day-2) — gated by admin role (web UI) */}
            <Route
              path="admin/users"
              element={
                <PrivateRoute requireAdmin>
                  <UsersPage />
                </PrivateRoute>
              }
            />
            <Route
              path="admin/operation-log"
              element={
                <PrivateRoute requireAdmin>
                  <OperationLogPage />
                </PrivateRoute>
              }
            />
          </Route>

          {/* Unknown path → home */}
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </Suspense>
    </BrowserRouter>
  );
}
