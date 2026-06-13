import { useState } from 'react';
import { PlusIcon, FolderIcon, DocumentTextIcon, Squares2X2Icon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { NamespaceDetail, NamespaceInfo } from '../../types/api';
import { listNamespaces, deleteNamespace } from '../../api/namespaces';
import {
  getNamespaceDetail,
  createNamespaceFull,
  updateNamespace,
} from '../../api/namespaceConfig';
import { parseAgentError } from '../../api/errors';
import { Button, Badge, notify } from '../ui';
import { LoadingSpinner } from '../Common/LoadingSpinner';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';
import { AdmissionError } from './AdmissionError';
import { NamespaceFormDialog, type NamespaceFormValues } from './NamespaceFormDialog';
import { NamespaceDetailDrawer } from './NamespaceDetailDrawer';
import { DeleteNamespaceDialog } from './DeleteNamespaceDialog';

// Namespace CRUD page (P02a design § 8 / F12). Stat cards + table → row click
// opens the detail drawer (fetches full 11-config detail) → Edit / Delete /
// Create dialogs. F05 admission errors on create are surfaced via AdmissionError
// (§ 8.4). TanStack Query owns the list cache; mutations invalidate + refetch.

function StatCard({
  label,
  value,
  icon: Icon,
  tone,
}: {
  label: string;
  value: React.ReactNode;
  icon: typeof FolderIcon;
  tone: string;
}) {
  return (
    <div className="rounded-xl border border-line bg-surface card-shadow p-5 transition-colors hover:border-magma/40">
      <div className="flex items-center justify-between">
        <span className="text-sm font-medium text-muted">{label}</span>
        <Icon className={`h-[18px] w-[18px] ${tone}`} aria-hidden="true" />
      </div>
      <div className="mt-2 font-mono text-3xl font-bold tracking-tight text-txt">{value}</div>
    </div>
  );
}

export function NamespacesPage() {
  const { t } = useTranslation();
  const qc = useQueryClient();

  const [createOpen, setCreateOpen] = useState(false);
  const [editTarget, setEditTarget] = useState<NamespaceDetail | null>(null);
  const [detailTarget, setDetailTarget] = useState<NamespaceDetail | null>(null);
  const [deleteTarget, setDeleteTarget] = useState<NamespaceDetail | null>(null);
  const [createError, setCreateError] = useState<ReturnType<typeof parseAgentError> | null>(null);

  const {
    data: namespaces = [],
    isLoading,
    isError,
    error,
    refetch,
  } = useQuery({ queryKey: ['namespaces'], queryFn: listNamespaces });

  const invalidate = () => {
    void qc.invalidateQueries({ queryKey: ['namespaces'] });
    void qc.invalidateQueries({ queryKey: ['namespace-detail'] });
  };

  const createMut = useMutation({
    mutationFn: (values: NamespaceFormValues) =>
      createNamespaceFull({
        name: values.name,
        description: values.description,
        isolation_mode: values.isolation_mode,
        visibility: values.visibility,
        configs: values.configs,
      }),
    onSuccess: () => {
      notify.success(t('namespace.created'));
      setCreateError(null);
      invalidate();
    },
    onError: (e) => setCreateError(parseAgentError(e)),
  });

  const updateMut = useMutation({
    mutationFn: (args: { name: string; values: NamespaceFormValues }) =>
      updateNamespace(args.name, {
        description: args.values.description,
        isolation_mode: args.values.isolation_mode,
        visibility: args.values.visibility,
        configs: args.values.configs,
      }),
    onSuccess: () => {
      notify.success(t('namespace.updated'));
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const deleteMut = useMutation({
    mutationFn: (ns: NamespaceDetail) => deleteNamespace(ns.name),
    onSuccess: () => {
      notify.success(t('namespace.deleted'));
      setDetailTarget(null);
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const openDetail = async (ns: NamespaceInfo) => {
    try {
      const detail = await getNamespaceDetail(ns.name);
      setDetailTarget(detail);
    } catch (e) {
      notify.error(parseAgentError(e).message);
    }
  };

  const totalDocs = namespaces.reduce((s, n) => s + n.doc_count, 0);
  const totalBlocks = namespaces.reduce((s, n) => s + n.block_count, 0);

  return (
    <div className="mx-auto max-w-6xl px-6 py-7 space-y-6">
      {/* Header */}
      <div className="flex items-end justify-between gap-4">
        <div>
          <p className="mb-1.5 text-xs font-semibold uppercase tracking-[3px] text-magma-h">
            Workspace
          </p>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">
            {t('sidebar.namespaces')}
          </h1>
          <p className="mt-1.5 text-sm text-muted">{t('namespace.subtitle')}</p>
        </div>
        <Button
          leftIcon={<PlusIcon className="h-5 w-5" />}
          onClick={() => {
            setCreateError(null);
            setCreateOpen(true);
          }}
          data-testid="namespace-new-btn"
        >
          {t('namespace.newNamespace')}
        </Button>
      </div>

      {/* Stat cards */}
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
        <StatCard
          label={t('sidebar.namespaces')}
          value={namespaces.length}
          icon={FolderIcon}
          tone="text-magma-h"
        />
        <StatCard
          label={t('namespace.documents')}
          value={totalDocs.toLocaleString()}
          icon={DocumentTextIcon}
          tone="text-amber"
        />
        <StatCard
          label={t('namespace.blocks')}
          value={totalBlocks.toLocaleString()}
          icon={Squares2X2Icon}
          tone="text-magma-h"
        />
      </div>

      <ProgrammaticBanner
        snippet={
          <>
            <span className="text-magma-h">kit</span>.
            <span className="text-amber">cortrix_namespace_create</span>(
            <span className="text-txt">name</span>=
            <span className="text-ok">&quot;production-docs&quot;</span>,{' '}
            <span className="text-txt">embedding</span>=
            <span className="text-ok">&quot;bge-m3&quot;</span>)
          </>
        }
      />

      {createError && <AdmissionError error={createError} onRetry={() => setCreateOpen(true)} />}

      {/* Table */}
      <div className="overflow-hidden rounded-xl border border-line bg-surface card-shadow">
        <div className="flex items-center justify-between border-b border-line px-5 py-3.5">
          <h2 className="text-[15px] font-semibold text-txt">{t('namespace.allNamespaces')}</h2>
        </div>

        {isLoading ? (
          <div className="flex justify-center py-16">
            <LoadingSpinner />
          </div>
        ) : isError ? (
          <div className="p-5">
            <AdmissionError error={parseAgentError(error)} onRetry={() => void refetch()} />
          </div>
        ) : namespaces.length === 0 ? (
          <div className="py-16 text-center text-sm text-muted">{t('namespace.empty')}</div>
        ) : (
          <table className="w-full text-sm">
            <thead>
              <tr className="border-b border-line text-left text-[11px] font-semibold uppercase tracking-wider text-muted">
                <th className="px-5 py-2.5">{t('sidebar.namespaces')}</th>
                <th className="px-5 py-2.5 text-right">{t('namespace.documents')}</th>
                <th className="px-5 py-2.5 text-right">{t('namespace.blocks')}</th>
                <th className="px-5 py-2.5">{t('namespace.status')}</th>
                <th className="px-5 py-2.5 text-right">{t('namespace.actions')}</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-line" data-testid="namespace-table">
              {namespaces.map((ns) => (
                <tr
                  key={ns.name}
                  data-namespace-name={ns.name}
                  className="cursor-pointer transition-colors hover:bg-magma/[0.05]"
                  onClick={() => void openDetail(ns)}
                >
                  <td className="px-5 py-3.5">
                    <div className="flex items-center gap-3">
                      <span className="grid h-8 w-8 place-items-center rounded-lg border border-magma/20 bg-magma/10 text-magma-h">
                        <FolderIcon className="h-[18px] w-[18px]" aria-hidden="true" />
                      </span>
                      <span className="font-semibold text-txt">{ns.name}</span>
                    </div>
                  </td>
                  <td className="px-5 py-3.5 text-right font-mono text-muted">
                    {ns.doc_count.toLocaleString()}
                  </td>
                  <td className="px-5 py-3.5 text-right font-mono text-muted">
                    {ns.block_count.toLocaleString()}
                  </td>
                  <td className="px-5 py-3.5">
                    <Badge variant={ns.doc_count > 0 ? 'ok' : 'warning'}>
                      {ns.doc_count > 0 ? 'Ready' : 'Empty'}
                    </Badge>
                  </td>
                  <td className="px-5 py-3.5 text-right">
                    <Button
                      size="sm"
                      variant="secondary"
                      onClick={(e) => {
                        e.stopPropagation();
                        void openDetail(ns);
                      }}
                      data-testid="namespace-view-btn"
                    >
                      {t('namespace.viewDetail')}
                    </Button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {/* Create / Edit dialogs */}
      <NamespaceFormDialog
        open={createOpen}
        onClose={() => setCreateOpen(false)}
        onSubmit={async (values) => {
          await createMut.mutateAsync(values);
        }}
      />
      <NamespaceFormDialog
        open={editTarget !== null}
        namespace={editTarget}
        onClose={() => setEditTarget(null)}
        onSubmit={async (values) => {
          await updateMut.mutateAsync({ name: editTarget!.name, values });
        }}
      />

      {/* Detail drawer */}
      <NamespaceDetailDrawer
        open={detailTarget !== null}
        namespace={detailTarget}
        onClose={() => setDetailTarget(null)}
        onEdit={(ns) => {
          setDetailTarget(null);
          setEditTarget(ns);
        }}
        onDelete={(ns) => setDeleteTarget(ns)}
      />

      {/* Delete confirm */}
      <DeleteNamespaceDialog
        open={deleteTarget !== null}
        namespace={deleteTarget}
        onClose={() => setDeleteTarget(null)}
        onConfirm={(ns) => deleteMut.mutateAsync(ns)}
      />
    </div>
  );
}
