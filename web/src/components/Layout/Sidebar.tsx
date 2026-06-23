import type { ComponentType, SVGProps } from 'react';
import { NavLink } from 'react-router-dom';
import {
  ArrowUpTrayIcon,
  MagnifyingGlassIcon,
  ChatBubbleLeftRightIcon,
  Cog6ToothIcon,
  FolderIcon,
  SparklesIcon,
  UsersIcon,
  ClipboardDocumentListIcon,
  HeartIcon,
  CircleStackIcon,
  ClipboardDocumentCheckIcon,
  BuildingOffice2Icon,
} from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useAuthStore } from '../../store/useAuthStore';
import { useFeatureFlags } from '../../hooks/useFeatureFlags';

type IconType = ComponentType<SVGProps<SVGSVGElement>>;

interface NavItem {
  to: string;
  label: string;
  icon: IconType;
  badge?: string;
  /** Exact match (used for the index "/" route so it isn't always active). */
  end?: boolean;
}

// Primary navigation (P02a design § 10 / § 9-bis). Migrated from the old
// `activePage` zustand selection to react-router <NavLink> in R3/S5 — the active
// state now derives from the URL. The Admin group (Users / Operation Log,
// § 9-bis) renders only for admin-role sessions. The Enterprise group renders
// only on enterprise builds (edition === 'enterprise'); CE hides it entirely
// (Derek 2026-06-23: the community edition must not advertise enterprise-only
// surfaces).

const PRIMARY: NavItem[] = [
  { to: '/namespaces', label: 'sidebar.namespaces', icon: FolderIcon },
  { to: '/upload', label: 'sidebar.upload', icon: ArrowUpTrayIcon },
  { to: '/search', label: 'sidebar.search', icon: MagnifyingGlassIcon, badge: 'DEV' },
  { to: '/agent', label: 'sidebar.chat', icon: ChatBubbleLeftRightIcon },
  { to: '/memory', label: 'sidebar.memory', icon: SparklesIcon },
  { to: '/health', label: 'sidebar.health', icon: HeartIcon },
];

const ADMIN: NavItem[] = [
  { to: '/admin/users', label: 'admin.users.navLabel', icon: UsersIcon },
  { to: '/admin/operation-log', label: 'admin.operations.navLabel', icon: ClipboardDocumentListIcon },
];

// Enterprise group (P02a § 6.3) — rendered only on enterprise builds. On the
// enterprise edition each item carries a "Coming" badge until its feature ships
// (V1.5); on CE the whole group is hidden (see Sidebar gating below).
const ENTERPRISE: NavItem[] = [
  { to: '/ent/text-to-sql', label: 'entPlaceholder.nav.textToSql', icon: CircleStackIcon, badge: 'Coming' },
  { to: '/ent/audit-log', label: 'entPlaceholder.nav.auditLog', icon: ClipboardDocumentCheckIcon, badge: 'Coming' },
  { to: '/ent/multi-tenant', label: 'entPlaceholder.nav.multiTenant', icon: BuildingOffice2Icon, badge: 'Coming' },
];

function navClass({ isActive }: { isActive: boolean }): string {
  return [
    'w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors duration-150',
    isActive
      ? 'bg-magma/10 text-magma-h font-semibold border border-magma/30'
      : 'text-muted hover:bg-surface2 hover:text-txt',
  ].join(' ');
}

function NavRow({ item }: { item: NavItem }) {
  const { t } = useTranslation();
  const Icon = item.icon;
  return (
    <NavLink to={item.to} end={item.end} className={navClass}>
      <Icon className="w-5 h-5 shrink-0" />
      <span className="truncate">{t(item.label)}</span>
      {item.badge && (
        <span className="shrink-0 text-[9px] font-bold px-1 py-0.5 rounded bg-amber/15 text-amber leading-none">
          {item.badge}
        </span>
      )}
    </NavLink>
  );
}

export function Sidebar() {
  const { t } = useTranslation();
  const isAdmin = useAuthStore((s) => s.currentUser?.role === 'admin');
  const { edition } = useFeatureFlags();
  const isEnterprise = edition === 'enterprise';

  return (
    <aside className="w-56 flex flex-col shrink-0 bg-bgalt border-r border-line">
      <nav className="flex-1 py-4 px-3 space-y-1 overflow-y-auto">
        {PRIMARY.map((item) => (
          <NavRow key={item.to} item={item} />
        ))}

        {isAdmin && (
          <div className="pt-4">
            <p className="px-3 pb-1.5 text-[10px] font-semibold uppercase tracking-wider text-muted">
              {t('admin.group')}
            </p>
            {ADMIN.map((item) => (
              <NavRow key={item.to} item={item} />
            ))}
          </div>
        )}

        {isEnterprise && (
          <div className="pt-4">
            <p className="px-3 pb-1.5 text-[10px] font-semibold uppercase tracking-wider text-muted">
              {t('entPlaceholder.group')}
            </p>
            {ENTERPRISE.map((item) => (
              <NavRow key={item.to} item={item} />
            ))}
          </div>
        )}
      </nav>

      {/* Settings at bottom */}
      <div className="p-3 border-t border-line">
        <NavLink to="/settings" className={navClass}>
          <Cog6ToothIcon className="w-5 h-5 shrink-0" />
          <span className="truncate">{t('sidebar.settings')}</span>
        </NavLink>
        <p className="mt-2 px-1 text-[11px] text-muted">{t('sidebar.tagline')}</p>
      </div>
    </aside>
  );
}
