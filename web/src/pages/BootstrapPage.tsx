import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { CheckIcon, ClipboardIcon, KeyIcon, ShieldExclamationIcon } from '@heroicons/react/24/outline';
import { bootstrap } from '../api/auth';
import { parseAgentError } from '../api/errors';
import { Button, Input, Card, CardHeader, CardTitle, CardBody, notify } from '../components/ui';
import { ProgrammaticBanner } from '../components/Common/ProgrammaticBanner';

// BootstrapPage (P02a design § 9.1 — Bootstrap URL flow). First-run setup: the
// `cortrix-setup` process prints a 60s one-time URL containing a token. This
// page exchanges that token via POST /api/v1/admin/bootstrap for the admin API
// key (the programmatic path; the GET path is a server-rendered HTML page).
// The token is consumed on first use by either endpoint. The returned key is
// shown once with a copy button + warning — the user stores it offline.

export function BootstrapPage() {
  const { t } = useTranslation();
  const [token, setToken] = useState('');
  const [adminKey, setAdminKey] = useState<string | null>(null);
  const [scopes, setScopes] = useState<string[]>([]);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const onSubmit = async () => {
    if (!token.trim()) return;
    setSubmitting(true);
    setError(null);
    try {
      const res = await bootstrap(token.trim());
      setAdminKey(res.admin_api_key);
      setScopes(res.scopes);
    } catch (e) {
      setError(parseAgentError(e).message);
    } finally {
      setSubmitting(false);
    }
  };

  const copyKey = async () => {
    if (!adminKey) return;
    try {
      await navigator.clipboard.writeText(adminKey);
      setCopied(true);
      notify.success(t('auth.bootstrap.copied'));
      setTimeout(() => setCopied(false), 2000);
    } catch {
      notify.error(t('auth.bootstrap.copyFailed'));
    }
  };

  return (
    <div className="min-h-full grid place-items-center bg-bg dots px-4 py-10">
      <div className="w-full max-w-lg space-y-5">
        <div className="flex items-center gap-3">
          <img src="/logo-mark.svg" alt="Cortrix" className="h-9 w-9" />
          <div>
            <h1 className="text-2xl font-extrabold tracking-tight text-txt">{t('auth.bootstrap.title')}</h1>
            <p className="text-sm text-muted">{t('auth.bootstrap.subtitle')}</p>
          </div>
        </div>

        {adminKey ? (
          <Card>
            <CardHeader>
              <CardTitle>
                <span className="inline-flex items-center gap-2">
                  <KeyIcon className="h-5 w-5 text-magma-h" aria-hidden="true" />
                  {t('auth.bootstrap.keyTitle')}
                </span>
              </CardTitle>
            </CardHeader>
            <CardBody className="space-y-4">
              <div
                className="flex items-center gap-3 rounded-md border border-amber/40 bg-amber/10 px-3 py-2 text-xs text-amber"
                role="alert"
              >
                <ShieldExclamationIcon className="h-4 w-4 shrink-0" aria-hidden="true" />
                {t('auth.bootstrap.warning')}
              </div>
              <div className="flex items-stretch gap-2">
                <code
                  className="flex-1 overflow-x-auto rounded-md border border-line bg-codebg px-3 py-2.5 font-mono text-[13px] text-txt"
                  data-testid="bootstrap-admin-key"
                >
                  {adminKey}
                </code>
                <Button
                  variant="secondary"
                  onClick={copyKey}
                  leftIcon={copied ? <CheckIcon className="h-4 w-4" /> : <ClipboardIcon className="h-4 w-4" />}
                  data-testid="bootstrap-copy-btn"
                >
                  {copied ? t('auth.bootstrap.copied') : t('common.copy')}
                </Button>
              </div>
              {scopes.length > 0 && (
                <p className="text-xs text-muted">
                  {t('auth.bootstrap.scopes')}: <span className="font-mono text-txt">{scopes.join(' · ')}</span>
                </p>
              )}
              <p className="text-xs text-muted">{t('auth.bootstrap.nextStep')}</p>
            </CardBody>
          </Card>
        ) : (
          <Card>
            <CardHeader>
              <CardTitle>{t('auth.bootstrap.exchangeTitle')}</CardTitle>
            </CardHeader>
            <CardBody className="space-y-4">
              <p className="text-sm text-muted">{t('auth.bootstrap.exchangeHint')}</p>
              <Input
                label={t('auth.bootstrap.tokenLabel')}
                value={token}
                onChange={(e) => setToken(e.target.value)}
                placeholder="ctx_bootstrap_…"
                error={error ?? undefined}
                onKeyDown={(e) => e.key === 'Enter' && void onSubmit()}
                data-testid="bootstrap-token-input"
                className="font-mono"
              />
              <Button
                onClick={() => void onSubmit()}
                loading={submitting}
                disabled={!token.trim()}
                className="w-full"
                data-testid="bootstrap-submit-btn"
              >
                {t('auth.bootstrap.exchange')}
              </Button>
            </CardBody>
          </Card>
        )}

        <ProgrammaticBanner
          comment={t('auth.bootstrap.sdkComment')}
          snippet={
            <>
              <span className="text-magma-h">client</span>.<span className="text-amber">admin</span>.
              <span className="text-amber">bootstrap</span>(<span className="text-txt">token</span>=
              <span className="text-ok">&quot;ctx_bootstrap_…&quot;</span>)
            </>
          }
        />
      </div>
    </div>
  );
}
