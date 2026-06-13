import { useState } from 'react';
import { useNavigate, useLocation } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useAuthStore } from '../store/useAuthStore';
import { parseAgentError } from '../api/errors';
import { Button, Input, Card, CardHeader, CardTitle, CardBody } from '../components/ui';
import { ProgrammaticBanner } from '../components/Common/ProgrammaticBanner';

// LoginPage (P02a design § 9.3). Email + password → useAuthStore.login, which
// POSTs to /api/v1/auth/login. The backend sets HttpOnly auth + CSRF cookies;
// the frontend never sees the token. On success we redirect to the page the
// user was trying to reach (PrivateRoute stashes it in location.state.from).

interface LocationState {
  from?: { pathname: string };
}

export function LoginPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const location = useLocation();
  const login = useAuthStore((s) => s.login);

  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const redirectTo = (location.state as LocationState | null)?.from?.pathname ?? '/';

  const onSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!email.trim() || !password) return;
    setSubmitting(true);
    setError(null);
    try {
      await login(email.trim(), password);
      navigate(redirectTo, { replace: true });
    } catch (err) {
      setError(parseAgentError(err).message);
    } finally {
      setSubmitting(false);
    }
  };

  return (
    <div className="min-h-full grid place-items-center bg-bg dots px-4 py-10">
      <div className="w-full max-w-md space-y-5">
        <div className="flex flex-col items-center gap-2 text-center">
          <img src="/logo-mark.svg" alt="Cortrix" className="h-10 w-10" />
          <h1 className="text-2xl font-extrabold tracking-tight text-txt">{t('auth.login.title')}</h1>
          <p className="text-sm text-muted">{t('auth.login.subtitle')}</p>
        </div>

        <Card>
          <CardHeader>
            <CardTitle className="text-lg">{t('auth.login.signIn')}</CardTitle>
          </CardHeader>
          <CardBody>
            <form className="space-y-4" onSubmit={onSubmit}>
              <Input
                id="login-email"
                type="email"
                label={t('auth.login.email')}
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                placeholder="you@example.com"
                autoComplete="username"
                data-testid="login-email"
              />
              <Input
                id="login-password"
                type="password"
                label={t('auth.login.password')}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="••••••••"
                autoComplete="current-password"
                error={error ?? undefined}
                data-testid="login-password"
              />
              <Button
                type="submit"
                loading={submitting}
                disabled={!email.trim() || !password}
                className="w-full"
                data-testid="login-submit"
              >
                {t('auth.login.signIn')}
              </Button>
            </form>
          </CardBody>
        </Card>

        <ProgrammaticBanner
          comment={t('auth.login.sdkComment')}
          channels="SDK · API Key"
          snippet={
            <>
              <span className="text-magma-h">client</span> = <span className="text-amber">Cortrix</span>(
              <span className="text-txt">api_key</span>=<span className="text-ok">&quot;cortrix_sk_…&quot;</span>)
            </>
          }
        />
      </div>
    </div>
  );
}
