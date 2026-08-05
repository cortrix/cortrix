import { MoonIcon, SunIcon, ArrowRightStartOnRectangleIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { useAppStore } from '../../store/useAppStore';
import { useAuthStore } from '../../store/useAuthStore';
import { NamespaceSelector } from '../Namespace/NamespaceSelector';

export function Header() {
  const { systemStatus, theme, toggleTheme } = useAppStore();
  const { currentUser, logout } = useAuthStore();
  const { t } = useTranslation();
  const navigate = useNavigate();

  const queue = systemStatus?.queue_status;

  const onLogout = async () => {
    await logout();
    navigate('/login', { replace: true });
  };

  return (
    <header className="h-14 flex items-center px-4 shrink-0 z-20 bg-bgalt/80 backdrop-blur border-b border-line">
      {/* Logo (Cortrix VI — Magma orange cube + wordmark) */}
      <div className="flex items-center gap-2.5 mr-6">
        <img src="/logo-mark.svg" alt="Cortrix" className="w-8 h-8 shrink-0" />
        <span className="text-lg font-extrabold tracking-tight text-txt">{t('header.title')}</span>
        <span className="text-xs text-muted font-mono ml-0.5">
          v{systemStatus?.version ?? '1.0.0-rc.1'}
        </span>
      </div>

      <NamespaceSelector />

      <div className="flex-1" />

      {/* System Status */}
      <div className="flex items-center gap-4 text-xs text-muted">
        <span className="flex items-center gap-1">
          <span className="w-1.5 h-1.5 rounded-full bg-ok animate-pulse" />
          {t('header.healthy')}
        </span>
        <span>
          {t('header.docs')}: <strong className="text-txt font-mono">{systemStatus?.total_doc_count ?? '—'}</strong>
        </span>
        <span>
          {t('header.blocks')}: <strong className="text-txt font-mono">{systemStatus?.total_block_count?.toLocaleString() ?? '—'}</strong>
        </span>
        {queue && (queue.pending > 0 || queue.processing > 0) && (
          <span>
            {t('header.queue')}: <strong className="text-magma-h font-mono">{t('header.pending', { count: queue.pending })}</strong>
          </span>
        )}
      </div>

      {/* LLM Status Badge — click to navigate to Settings page */}
      {systemStatus?.llm_enabled ? (
        <button
          onClick={() => navigate('/settings')}
          className="ml-4 px-2.5 py-0.5 text-xs rounded-full font-medium bg-ok/10 text-ok flex items-center gap-1 cursor-pointer hover:bg-ok/20"
        >
          <span className="w-1.5 h-1.5 rounded-full bg-ok" />
          {t('llm.status')}: {systemStatus.llm_provider}
        </button>
      ) : (
        <button
          onClick={() => navigate('/settings')}
          className="ml-4 px-2.5 py-0.5 text-xs rounded-full font-medium bg-surface2 text-muted flex items-center gap-1 cursor-pointer hover:text-txt"
        >
          <span className="w-1.5 h-1.5 rounded-full bg-muted" />
          {t('llm.status')}: {t('llm.off')}
        </button>
      )}

      {/* Current user + logout (web UI — HttpOnly cookie session) */}
      {currentUser && (
        <div className="ml-4 flex items-center gap-2">
          <span
            className="hidden md:inline text-xs text-muted"
            data-testid="current-user"
            data-user-role={currentUser.role}
          >
            {currentUser.email}
          </span>
          <button
            onClick={() => void onLogout()}
            title={t('auth.logout')}
            aria-label={t('auth.logout')}
            data-testid="logout-btn"
            className="w-9 h-9 grid place-items-center rounded-lg border border-line bg-surface text-muted transition-colors hover:text-magma-h hover:border-magma/40"
          >
            <ArrowRightStartOnRectangleIcon className="w-5 h-5" />
          </button>
        </div>
      )}

      {/* Theme Toggle (Magma VI — sun/moon, mockup topbar style) */}
      <button
        onClick={toggleTheme}
        className="ml-4 w-9 h-9 grid place-items-center rounded-lg border border-line bg-surface text-muted transition-colors hover:text-magma-h hover:border-magma/40"
        title={t('header.toggleTheme')}
      >
        {theme === 'dark' ? <SunIcon className="w-5 h-5" /> : <MoonIcon className="w-5 h-5" />}
      </button>

    </header>
  );
}
