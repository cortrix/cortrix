import { useTranslation } from 'react-i18next';
import {
  CircleStackIcon,
  ClipboardDocumentCheckIcon,
  BuildingOffice2Icon,
} from '@heroicons/react/24/outline';
import { useFeatureFlags } from '../../hooks/useFeatureFlags';
import { PlaceholderPage, type PlaceholderMarketingLinks } from './PlaceholderPage';

// Ent feature gate pages (P02a design § 6.2 / § 10.2). Each page reads the
// feature flag store: when the backend reports the feature enabled it renders
// the real component (V1.5 — not bundled in V1.0), when it is a placeholder it
// renders the marketing PlaceholderPage. The routes are registered in V1.0
// already (§ 10.1) so bookmarks survive the V1.5 swap.

const MARKETING: PlaceholderMarketingLinks = {
  roadmap: 'https://cortrix.ai/roadmap',
  cloudBeta: 'https://cortrix.ai/cloud-v1-signup',
  contact: 'mailto:enterprise@cortrix.ai',
};

// V1.0 stub — the real component ships in V1.5 (lazy chunk, § 14.3). Until then
// an enabled flag with no real component still falls through to the placeholder.
function notYetBundled(): null {
  return null;
}

export function TextToSqlPage() {
  const { t } = useTranslation();
  const { isEnabled } = useFeatureFlags();
  if (isEnabled('text_to_sql')) return notYetBundled();
  return (
    <PlaceholderPage
      featureKey="text_to_sql"
      icon={CircleStackIcon}
      title={t('entPlaceholder.textToSql.title')}
      description={t('entPlaceholder.textToSql.description')}
      problemSolved={t('entPlaceholder.textToSql.problemSolved')}
      useCases={t('entPlaceholder.textToSql.useCases', { returnObjects: true }) as string[]}
      availableIn={t('entPlaceholder.textToSql.availableIn')}
      marketingLinks={MARKETING}
    />
  );
}

export function AuditLogPage() {
  const { t } = useTranslation();
  const { isEnabled } = useFeatureFlags();
  if (isEnabled('audit_log_ent')) return notYetBundled();
  return (
    <PlaceholderPage
      featureKey="audit_log_ent"
      icon={ClipboardDocumentCheckIcon}
      title={t('entPlaceholder.auditLog.title')}
      description={t('entPlaceholder.auditLog.description')}
      problemSolved={t('entPlaceholder.auditLog.problemSolved')}
      useCases={t('entPlaceholder.auditLog.useCases', { returnObjects: true }) as string[]}
      availableIn={t('entPlaceholder.auditLog.availableIn')}
      marketingLinks={MARKETING}
    />
  );
}

export function MultiTenantPage() {
  const { t } = useTranslation();
  const { isEnabled } = useFeatureFlags();
  if (isEnabled('multi_tenant_switch')) return notYetBundled();
  return (
    <PlaceholderPage
      featureKey="multi_tenant_switch"
      icon={BuildingOffice2Icon}
      title={t('entPlaceholder.multiTenant.title')}
      description={t('entPlaceholder.multiTenant.description')}
      problemSolved={t('entPlaceholder.multiTenant.problemSolved')}
      useCases={t('entPlaceholder.multiTenant.useCases', { returnObjects: true }) as string[]}
      availableIn={t('entPlaceholder.multiTenant.availableIn')}
      marketingLinks={MARKETING}
    />
  );
}
