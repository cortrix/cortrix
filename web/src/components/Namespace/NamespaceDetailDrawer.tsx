import { PencilIcon, TrashIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { NamespaceDetail } from '../../types/api';
import { Drawer } from '../Common/Drawer';
import { Badge, Button } from '../ui';
import { formatFileSize } from '../../utils/formatters';
import { CONFIG_META } from './namespaceConfigMeta';

// NamespaceDetailDrawer (P02a design § 8.5) — right slide-over showing metadata
// + all 11 *_config as collapsible read-only JSON accordions + F05 admission
// state, with Edit / Delete actions at the bottom. Does not disturb the list.

interface NamespaceDetailDrawerProps {
  open: boolean;
  namespace: NamespaceDetail | null;
  onClose: () => void;
  onEdit: (ns: NamespaceDetail) => void;
  onDelete: (ns: NamespaceDetail) => void;
}

function MetaRow({ label, value }: { label: string; value: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-4 py-1.5 text-sm">
      <span className="text-muted">{label}</span>
      <span className="truncate font-medium text-txt">{value}</span>
    </div>
  );
}

export function NamespaceDetailDrawer({
  open,
  namespace,
  onClose,
  onEdit,
  onDelete,
}: NamespaceDetailDrawerProps) {
  const { t } = useTranslation();
  const ns = namespace;

  return (
    <Drawer
      open={open}
      onClose={onClose}
      title={ns?.name ?? t('namespace.detailTitle')}
      subtitle={ns ? <span className="font-mono">{ns.ns_id}</span> : undefined}
      footer={
        ns ? (
          <>
            <Button
              variant="secondary"
              leftIcon={<TrashIcon className="h-4 w-4" />}
              onClick={() => onDelete(ns)}
              data-testid="drawer-delete-btn"
            >
              {t('common.delete')}
            </Button>
            <Button
              leftIcon={<PencilIcon className="h-4 w-4" />}
              onClick={() => onEdit(ns)}
              data-testid="drawer-edit-btn"
            >
              {t('common.edit')}
            </Button>
          </>
        ) : undefined
      }
    >
      {ns && (
        <div className="space-y-6">
          {/* Metadata */}
          <section>
            <h3 className="mb-2 text-xs font-semibold uppercase tracking-wider text-muted">
              {t('namespace.metadata')}
            </h3>
            <div className="divide-y divide-line rounded-lg border border-line bg-surface px-4">
              {ns.description && <MetaRow label={t('namespace.descriptionLabel')} value={ns.description} />}
              <MetaRow label={t('namespace.isolationMode')} value={ns.isolation_mode} />
              <MetaRow label={t('namespace.visibility')} value={ns.visibility} />
              <MetaRow label={t('namespace.status')} value={<Badge variant={ns.status === 'active' ? 'ok' : 'error'}>{ns.status}</Badge>} />
              <MetaRow label={t('namespace.documents')} value={<span className="font-mono">{(ns.doc_count ?? 0).toLocaleString()}</span>} />
              <MetaRow label={t('namespace.blocks')} value={<span className="font-mono">{(ns.block_count ?? 0).toLocaleString()}</span>} />
            </div>
          </section>

          {/* F05 admission state */}
          {ns.admission && (
            <section>
              <h3 className="mb-2 text-xs font-semibold uppercase tracking-wider text-muted">
                {t('namespace.admissionState')}
              </h3>
              <div className="divide-y divide-line rounded-lg border border-line bg-surface px-4">
                {ns.admission.estimated_size_bytes != null && (
                  <MetaRow
                    label="Estimated size"
                    value={<span className="font-mono">{formatFileSize(ns.admission.estimated_size_bytes)}</span>}
                  />
                )}
                {ns.admission.budget_limit_bytes != null && (
                  <MetaRow
                    label="Budget limit"
                    value={<span className="font-mono">{formatFileSize(ns.admission.budget_limit_bytes)}</span>}
                  />
                )}
                {ns.admission.quota_used != null && ns.admission.quota_limit != null && (
                  <MetaRow
                    label="Namespace quota"
                    value={
                      <span className="font-mono">
                        {ns.admission.quota_used} / {ns.admission.quota_limit}
                      </span>
                    }
                  />
                )}
              </div>
            </section>
          )}

          {/* 11 *_config accordions */}
          <section>
            <h3 className="mb-2 text-xs font-semibold uppercase tracking-wider text-muted">
              Configs
            </h3>
            <div className="space-y-2">
              {CONFIG_META.map((meta) => {
                // Defensive: the backend (F12 BuildNamespaceJson) may omit the
                // whole `configs` object or individual *_config keys. Optional-
                // chain through both so a missing field degrades to a "default"
                // accordion instead of throwing (undefined[key]) into the
                // ErrorBoundary.
                const cfg = ns.configs?.[meta.key] ?? {};
                const empty = Object.keys(cfg).length === 0;
                return (
                  <details
                    key={meta.key}
                    className="rounded-lg border border-line bg-surface"
                    data-testid={`drawer-config-${meta.key}`}
                  >
                    <summary className="flex cursor-pointer items-center justify-between gap-2 px-4 py-2.5 text-sm">
                      <span className="flex items-center gap-2">
                        <span className="font-medium text-txt">{meta.label}</span>
                      </span>
                      <Badge variant={empty ? 'warning' : 'ok'}>
                        {empty ? 'default' : 'configured'}
                      </Badge>
                    </summary>
                    <pre className="overflow-auto border-t border-line bg-codebg px-4 py-3 font-mono text-[11px] text-txt">
                      {JSON.stringify(cfg, null, 2)}
                    </pre>
                  </details>
                );
              })}
            </div>
          </section>
        </div>
      )}
    </Drawer>
  );
}
