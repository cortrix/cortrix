import { useEffect } from 'react';
import { useLocation } from 'react-router-dom';
import { recordPageView } from './metrics';

// Page-view metric hook (web UI design — cortrix_webui_page_views_total).
// Mounted inside the router (Layout) so it fires once per route change with the
// normalized top-level page label (login/home/memory/ns/admin/...). Best-effort:
// recordPageView is a no-op before metrics init / when the collector is down.

/** Normalize a pathname to the low-cardinality `page` label from */
export function pageLabel(pathname: string): string {
  if (pathname === '/' || pathname === '') return 'home';
  const seg = pathname.replace(/^\//, '').split('/');
  if (seg[0] === 'namespaces') return 'ns';
  if (seg[0] === 'admin') return `admin:${seg[1] ?? ''}`.replace(/:$/, '');
  if (seg[0] === 'ent') return `ent:${seg[1] ?? ''}`.replace(/:$/, '');
  return seg[0];
}

export function usePageViews(): void {
  const location = useLocation();
  useEffect(() => {
    recordPageView(pageLabel(location.pathname));
  }, [location.pathname]);
}
