import { useEffect } from 'react';
import { Navigate, useLocation } from 'react-router-dom';
import type { ReactNode } from 'react';
import { useAuthStore } from '../store/useAuthStore';
import { LoadingSpinner } from '../components/Common/LoadingSpinner';

// PrivateRoute (P02a design § 9.3 — route guard). Wraps protected routes:
//   - On first mount it runs the cookie probe (GET /api/v1/auth/me) once.
//   - While the probe is in flight it shows a spinner (avoids a flash redirect).
//   - Unauthenticated → redirect to /login, remembering the attempted path so
//     LoginPage can send the user back after a successful sign-in.
//
// Optionally `requireAdmin` gates admin-only pages (Users / Operation Log);
// a non-admin authenticated user is redirected home rather than to /login.

interface PrivateRouteProps {
  children: ReactNode;
  requireAdmin?: boolean;
}

export function PrivateRoute({ children, requireAdmin = false }: PrivateRouteProps) {
  const location = useLocation();
  const { isAuthenticated, authChecked, currentUser, probe } = useAuthStore();

  useEffect(() => {
    if (!authChecked) void probe();
  }, [authChecked, probe]);

  if (!authChecked) {
    return (
      <div className="grid min-h-full place-items-center bg-bg">
        <LoadingSpinner />
      </div>
    );
  }

  if (!isAuthenticated) {
    return <Navigate to="/login" replace state={{ from: location }} />;
  }

  if (requireAdmin && currentUser?.role !== 'admin') {
    return <Navigate to="/" replace />;
  }

  return <>{children}</>;
}
