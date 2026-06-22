import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import {
  PlusIcon,
  UserIcon,
  PencilIcon,
  NoSymbolIcon,
  CheckCircleIcon,
  MagnifyingGlassIcon,
} from '@heroicons/react/24/outline';
import type { UserRecord, UserRole, UserStatus } from '../../types/api';
import { listUsers, createUser, updateUser, disableUser, enableUser } from '../../api/admin';
import { parseAgentError } from '../../api/errors';
import { useAuthStore } from '../../store/useAuthStore';
import { Button, Badge, Input, Select, notify } from '../../components/ui';
import { LoadingSpinner } from '../../components/Common/LoadingSpinner';
import { ProgrammaticBanner } from '../../components/Common/ProgrammaticBanner';
import { ErrorDisplay } from '../../components/Common/ErrorDisplay';
import { UserFormModal, type UserFormValues } from './UserFormModal';

// UsersPage (P02a design § 9-bis.1 — Day-2 admin, CE). Integrates the P08
// admin/users 5-endpoint set (list / create / update / disable / enable) via
// TanStack Query. Note (P02a-9bis-3 / V6 A30): there is NO hard delete — the
// disable+enable pair is the lifecycle, so disabled users are retained. Access
// is gated by PrivateRoute requireAdmin; this page also reads the current role
// to keep self-service safety (an admin can't disable their own account here).

const PAGE_SIZE = 20;

function formatCreatedDate(value?: string | number): string {
  if (value == null) return '—';
  if (typeof value === 'number') {
    const millis = value < 1_000_000_000_000 ? value * 1000 : value;
    const date = new Date(millis);
    return Number.isNaN(date.getTime()) ? '—' : date.toISOString().slice(0, 10);
  }
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value.slice(0, 10) : date.toISOString().slice(0, 10);
}

function StatusBadge({ status }: { status: UserStatus }) {
  const { t } = useTranslation();
  return (
    <Badge variant={status === 'active' ? 'ok' : 'warning'}>
      {status === 'active' ? t('admin.users.statusActive') : t('admin.users.statusDisabled')}
    </Badge>
  );
}

export function UsersPage() {
  const { t } = useTranslation();
  const qc = useQueryClient();
  const currentUser = useAuthStore((s) => s.currentUser);

  const [q, setQ] = useState('');
  const [roleFilter, setRoleFilter] = useState<'all' | UserRole>('all');
  const [statusFilter, setStatusFilter] = useState<'all' | UserStatus>('all');
  const [page, setPage] = useState(1);
  const [createOpen, setCreateOpen] = useState(false);
  const [editTarget, setEditTarget] = useState<UserRecord | null>(null);

  const filter = {
    q: q || undefined,
    role: roleFilter === 'all' ? undefined : roleFilter,
    status: statusFilter === 'all' ? undefined : statusFilter,
    page,
    limit: PAGE_SIZE,
  };

  const { data, isLoading, isError, error, refetch } = useQuery({
    queryKey: ['admin-users', filter],
    queryFn: () => listUsers(filter),
  });

  const invalidate = () => void qc.invalidateQueries({ queryKey: ['admin-users'] });

  const createMut = useMutation({
    mutationFn: (values: UserFormValues) =>
      createUser({ email: values.email, password: values.password, role: values.role, display_name: values.display_name }),
    onSuccess: () => {
      notify.success(t('admin.users.createdMsg'));
      setCreateOpen(false);
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const updateMut = useMutation({
    mutationFn: (args: { id: string; values: UserFormValues }) =>
      updateUser(args.id, { email: args.values.email, role: args.values.role, display_name: args.values.display_name }),
    onSuccess: () => {
      notify.success(t('admin.users.updated'));
      setEditTarget(null);
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const statusMut = useMutation({
    mutationFn: (u: UserRecord) => (u.status === 'active' ? disableUser(u.id) : enableUser(u.id)),
    onSuccess: (_res, u) => {
      notify.success(u.status === 'active' ? t('admin.users.disabled') : t('admin.users.enabled'));
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const users = data?.users ?? [];
  const total = data?.total ?? 0;
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));

  const roleOptions = [
    { value: 'all' as const, label: t('admin.users.allRoles') },
    { value: 'user' as const, label: t('admin.users.roleUser') },
    { value: 'admin' as const, label: t('admin.users.roleAdmin') },
  ];
  const statusOptions = [
    { value: 'all' as const, label: t('admin.users.allStatuses') },
    { value: 'active' as const, label: t('admin.users.statusActive') },
    { value: 'disabled' as const, label: t('admin.users.statusDisabled') },
  ];

  return (
    <div className="mx-auto max-w-6xl px-6 py-7 space-y-6">
      <div className="flex items-end justify-between gap-4">
        <div>
          <p className="mb-1.5 text-xs font-semibold uppercase tracking-[3px] text-magma-h">{t('admin.group')}</p>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">{t('admin.users.title')}</h1>
          <p className="mt-1.5 text-sm text-muted">{t('admin.users.subtitle')}</p>
        </div>
        <Button leftIcon={<PlusIcon className="h-5 w-5" />} onClick={() => setCreateOpen(true)} data-testid="user-new-btn">
          {t('admin.users.newUser')}
        </Button>
      </div>

      <ProgrammaticBanner
        comment={t('admin.users.sdkComment')}
        snippet={
          <>
            <span className="text-magma-h">client</span>.<span className="text-amber">admin</span>.
            <span className="text-amber">users</span>.<span className="text-amber">create</span>(
            <span className="text-txt">email</span>=<span className="text-ok">&quot;alice@corp.com&quot;</span>,{' '}
            <span className="text-txt">role</span>=<span className="text-ok">&quot;user&quot;</span>)
          </>
        }
      />

      {/* Filters */}
      <div className="flex flex-wrap items-end gap-3">
        <div className="relative min-w-[220px] flex-1">
          <MagnifyingGlassIcon className="pointer-events-none absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-muted" />
          <Input
            value={q}
            onChange={(e) => {
              setQ(e.target.value);
              setPage(1);
            }}
            placeholder={t('admin.users.searchPlaceholder')}
            className="pl-9"
            data-testid="user-search-input"
          />
        </div>
        <div className="w-40">
          <Select<'all' | UserRole>
            value={roleFilter}
            onChange={(v) => {
              setRoleFilter(v);
              setPage(1);
            }}
            options={roleOptions}
          />
        </div>
        <div className="w-40">
          <Select<'all' | UserStatus>
            value={statusFilter}
            onChange={(v) => {
              setStatusFilter(v);
              setPage(1);
            }}
            options={statusOptions}
          />
        </div>
      </div>

      {/* Table */}
      <div className="overflow-hidden rounded-xl border border-line bg-surface card-shadow">
        <div className="flex items-center justify-between border-b border-line px-5 py-3.5">
          <h2 className="text-[15px] font-semibold text-txt">{t('admin.users.allUsers')}</h2>
          <span className="font-mono text-xs text-muted">{t('admin.users.total', { count: total })}</span>
        </div>

        {isLoading ? (
          <div className="flex justify-center py-16">
            <LoadingSpinner />
          </div>
        ) : isError ? (
          <div className="p-5">
            <ErrorDisplay error={parseAgentError(error)} onRetry={() => void refetch()} />
          </div>
        ) : users.length === 0 ? (
          <div className="py-16 text-center text-sm text-muted">{t('admin.users.empty')}</div>
        ) : (
          <table className="w-full text-sm">
            <thead>
              <tr className="border-b border-line text-left text-[11px] font-semibold uppercase tracking-wider text-muted">
                <th className="px-5 py-2.5">{t('admin.users.email')}</th>
                <th className="px-5 py-2.5">{t('admin.users.role')}</th>
                <th className="px-5 py-2.5">{t('admin.users.status')}</th>
                <th className="px-5 py-2.5">{t('admin.users.created')}</th>
                <th className="px-5 py-2.5 text-right">{t('admin.users.actions')}</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-line" data-testid="users-table">
              {users.map((u) => {
                const isSelf = u.id === currentUser?.id;
                return (
                  <tr key={u.id} data-user-id={u.id} data-user-email={u.email} className="transition-colors hover:bg-magma/[0.05]">
                    <td className="px-5 py-3.5">
                      <div className="flex items-center gap-3">
                        <span className="grid h-8 w-8 place-items-center rounded-lg border border-magma/20 bg-magma/10 text-magma-h">
                          <UserIcon className="h-[18px] w-[18px]" aria-hidden="true" />
                        </span>
                        <div className="min-w-0">
                          <div className="truncate font-semibold text-txt">{u.email}</div>
                          {u.display_name && <div className="truncate text-xs text-muted">{u.display_name}</div>}
                        </div>
                      </div>
                    </td>
                    <td className="px-5 py-3.5">
                      <Badge variant={u.role === 'admin' ? 'magma' : 'primary'}>
                        {u.role === 'admin' ? t('admin.users.roleAdmin') : t('admin.users.roleUser')}
                      </Badge>
                    </td>
                    <td className="px-5 py-3.5">
                      <StatusBadge status={u.status} />
                    </td>
                    <td className="px-5 py-3.5 font-mono text-xs text-muted">
                      {formatCreatedDate(u.created_at)}
                    </td>
                    <td className="px-5 py-3.5">
                      <div className="flex items-center justify-end gap-2">
                        <Button
                          size="sm"
                          variant="secondary"
                          leftIcon={<PencilIcon className="h-4 w-4" />}
                          onClick={() => setEditTarget(u)}
                          data-testid="user-edit-btn"
                        >
                          {t('common.edit')}
                        </Button>
                        <Button
                          size="sm"
                          variant={u.status === 'active' ? 'danger' : 'secondary'}
                          loading={statusMut.isPending && statusMut.variables?.id === u.id}
                          disabled={isSelf}
                          title={isSelf ? t('admin.users.cannotDisableSelf') : undefined}
                          leftIcon={
                            u.status === 'active' ? (
                              <NoSymbolIcon className="h-4 w-4" />
                            ) : (
                              <CheckCircleIcon className="h-4 w-4" />
                            )
                          }
                          onClick={() => statusMut.mutate(u)}
                          data-testid="user-toggle-status-btn"
                        >
                          {u.status === 'active' ? t('admin.users.disable') : t('admin.users.enable')}
                        </Button>
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}

        {/* Pagination */}
        {!isLoading && !isError && total > PAGE_SIZE && (
          <div className="flex items-center justify-between border-t border-line px-5 py-3">
            <span className="text-xs text-muted">
              {t('admin.users.pageOf', { page, total: totalPages })}
            </span>
            <div className="flex gap-2">
              <Button size="sm" variant="secondary" disabled={page <= 1} onClick={() => setPage((p) => p - 1)}>
                {t('common.previous')}
              </Button>
              <Button
                size="sm"
                variant="secondary"
                disabled={page >= totalPages}
                onClick={() => setPage((p) => p + 1)}
              >
                {t('common.next')}
              </Button>
            </div>
          </div>
        )}
      </div>

      <UserFormModal
        open={createOpen}
        submitting={createMut.isPending}
        onClose={() => setCreateOpen(false)}
        onSubmit={async (values) => {
          await createMut.mutateAsync(values);
        }}
      />
      <UserFormModal
        open={editTarget !== null}
        user={editTarget}
        submitting={updateMut.isPending}
        onClose={() => setEditTarget(null)}
        onSubmit={async (values) => {
          await updateMut.mutateAsync({ id: editTarget!.id, values });
        }}
      />
    </div>
  );
}
