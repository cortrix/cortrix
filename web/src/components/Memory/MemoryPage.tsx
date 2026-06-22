import { useState } from 'react';
import { Switch } from '@headlessui/react';
import { PlusIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { MemoryItem, MemoryType } from '../../types/api';
import { listMemory, createMemory, editMemory, invalidateMemory } from '../../api/memory';
import { parseAgentError } from '../../api/errors';
import { Button, Input, Select, notify } from '../ui';
import { LoadingSpinner } from '../Common/LoadingSpinner';
import { ErrorDisplay } from '../Common/ErrorDisplay';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';
import { MemoryCard } from './MemoryCard';
import { MemoryFormDialog } from './MemoryFormDialog';
import { InvalidateMemoryDialog } from './InvalidateMemoryDialog';
import { useAppStore } from '../../store/useAppStore';

// Memory CRUD page (P02a design § 7 / MEM03). Toolbar (user_id + type filter +
// explain toggle + show-invalidated toggle + Create) → card list → pagination,
// plus 3 dialogs (create / edit / invalidate). Data via TanStack Query so
// mutations invalidate the list cache and refetch automatically.

const PAGE_SIZE = 20;

const TYPE_OPTIONS: { value: MemoryType | ''; label: string }[] = [
  { value: '', label: 'All types' },
  { value: 'fact', label: 'fact' },
  { value: 'preference', label: 'preference' },
  { value: 'event', label: 'event' },
];

function ToggleRow({
  label,
  hint,
  checked,
  onChange,
  testId,
}: {
  label: string;
  hint?: string;
  checked: boolean;
  onChange: (v: boolean) => void;
  testId: string;
}) {
  return (
    <div className="flex items-center gap-2">
      <Switch
        checked={checked}
        onChange={onChange}
        data-testid={testId}
        className={`${
          checked ? 'bg-magma' : 'bg-surface2'
        } relative inline-flex h-5 w-9 shrink-0 items-center rounded-full border border-line transition-colors`}
        title={hint}
      >
        <span
          className={`${
            checked ? 'translate-x-4' : 'translate-x-0.5'
          } inline-block h-3.5 w-3.5 transform rounded-full bg-white transition-transform`}
        />
      </Switch>
      <span className="text-sm text-txt">{label}</span>
    </div>
  );
}

export function MemoryPage() {
  const { t } = useTranslation();
  const qc = useQueryClient();
  const currentNamespace = useAppStore((s) => s.currentNamespace);
  const namespaceReady = useAppStore((s) =>
    s.namespaces.some((ns) => ns.name === s.currentNamespace),
  );

  const [userId, setUserId] = useState('user_demo');
  const [memType, setMemType] = useState<MemoryType | ''>('');
  const [explain, setExplain] = useState(false);
  const [showInvalidated, setShowInvalidated] = useState(false);
  const [page, setPage] = useState(0);

  const [createOpen, setCreateOpen] = useState(false);
  const [editTarget, setEditTarget] = useState<MemoryItem | null>(null);
  const [invalidateTarget, setInvalidateTarget] = useState<MemoryItem | null>(null);

  const filter = {
    namespace: currentNamespace,
    user_id: userId,
    include_invalidated: showInvalidated,
    memory_type: memType || undefined,
    explain,
    page,
    page_size: PAGE_SIZE,
    limit: PAGE_SIZE,
    offset: page * PAGE_SIZE,
  };

  const queryKey = ['memory', filter];
  const { data, isLoading, isError, error, refetch, isFetching } = useQuery({
    queryKey,
    queryFn: () => listMemory(filter),
    enabled: userId.trim().length > 0 && namespaceReady,
  });

  const invalidateList = () => qc.invalidateQueries({ queryKey: ['memory'] });

  const createMut = useMutation({
    mutationFn: (values: { content: string; memory_type: MemoryType }) =>
      createMemory({ namespace: currentNamespace, user_id: userId, ...values }),
    onSuccess: () => {
      notify.success(t('memory.created'));
      void invalidateList();
    },
  });

  const editMut = useMutation({
    mutationFn: (args: { id: string; content: string; memory_type: MemoryType }) =>
      editMemory(args.id, {
        namespace: currentNamespace,
        user_id: userId,
        content: args.content,
        memory_type: args.memory_type,
        new_content: args.content,
        new_memory_type: args.memory_type,
      }),
    onSuccess: () => {
      notify.success(t('memory.edited'));
      void invalidateList();
    },
  });

  const invalidateMut = useMutation({
    mutationFn: (m: MemoryItem) =>
      invalidateMemory(m.memory_id, { namespace: currentNamespace, user_id: userId }),
    onSuccess: () => {
      notify.success(t('memory.invalidated'));
      void invalidateList();
    },
  });

  const memories = data?.memories ?? [];
  const total = data?.meta.total ?? 0;
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));

  return (
    <div className="mx-auto max-w-5xl px-6 py-7 space-y-6">
      {/* Header */}
      <div className="flex items-end justify-between gap-4">
        <div>
          <p className="mb-1.5 text-xs font-semibold uppercase tracking-[3px] text-magma-h">
            Intelligence
          </p>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">{t('memory.title')}</h1>
          <p className="mt-1.5 text-sm text-muted">{t('memory.subtitle')}</p>
        </div>
        <Button
          leftIcon={<PlusIcon className="h-5 w-5" />}
          onClick={() => setCreateOpen(true)}
          data-testid="memory-new-btn"
        >
          {t('memory.new')}
        </Button>
      </div>

      <ProgrammaticBanner
        comment="Every action here has a programmatic equivalent"
        snippet={
          <>
            <span className="text-magma-h">kit</span>.
            <span className="text-amber">cortrix_memory_list</span>(
            <span className="text-txt">user_id</span>=
            <span className="text-ok">&quot;{userId || 'user_demo'}&quot;</span>,{' '}
            <span className="text-txt">explain</span>=
            <span className="text-ok">{String(explain)}</span>)
          </>
        }
      />

      {/* Toolbar */}
      <div className="flex flex-wrap items-end gap-4 rounded-xl border border-line bg-surface card-shadow p-4">
        <Input
          label={t('memory.userId')}
          value={userId}
          onChange={(e) => {
            setUserId(e.target.value);
            setPage(0);
          }}
          className="font-mono"
          data-testid="memory-user-filter"
        />
        <div className="w-44">
          <Select<MemoryType | ''>
            label={t('memory.type')}
            value={memType}
            onChange={(v) => {
              setMemType(v);
              setPage(0);
            }}
            options={TYPE_OPTIONS}
          />
        </div>
        <div className="flex flex-col gap-2 pb-1">
          <ToggleRow
            label={t('memory.explainMode')}
            hint={t('memory.explainHint')}
            checked={explain}
            onChange={setExplain}
            testId="memory-explain-toggle"
          />
          <ToggleRow
            label={t('memory.showInvalidated')}
            checked={showInvalidated}
            onChange={(v) => {
              setShowInvalidated(v);
              setPage(0);
            }}
            testId="memory-invalidated-toggle"
          />
        </div>
      </div>

      {/* List */}
      {isLoading ? (
        <div className="flex justify-center py-16">
          <LoadingSpinner />
        </div>
      ) : isError ? (
        <ErrorDisplay error={parseAgentError(error)} onRetry={() => void refetch()} />
      ) : memories.length === 0 ? (
        <div className="rounded-xl border border-dashed border-line bg-surface py-16 text-center text-sm text-muted">
          {t('memory.empty')}
        </div>
      ) : (
        <div className="space-y-3" data-testid="memory-list" aria-busy={isFetching}>
          {memories.map((m) => (
            <MemoryCard
              key={m.memory_id}
              memory={m}
              explain={explain}
              onEdit={setEditTarget}
              onInvalidate={setInvalidateTarget}
            />
          ))}
        </div>
      )}

      {/* Pagination */}
      {total > PAGE_SIZE && (
        <div className="flex items-center justify-between text-sm text-muted">
          <span className="font-mono">
            {page * PAGE_SIZE + 1}–{Math.min((page + 1) * PAGE_SIZE, total)} / {total}
          </span>
          <div className="flex gap-2">
            <Button
              size="sm"
              variant="secondary"
              disabled={page === 0}
              onClick={() => setPage((p) => Math.max(0, p - 1))}
            >
              {t('common.previous')}
            </Button>
            <Button
              size="sm"
              variant="secondary"
              disabled={page >= totalPages - 1}
              onClick={() => setPage((p) => p + 1)}
            >
              {t('common.next')}
            </Button>
          </div>
        </div>
      )}

      {/* Dialogs */}
      <MemoryFormDialog
        open={createOpen}
        onClose={() => setCreateOpen(false)}
        onSubmit={async (values) => {
          await createMut.mutateAsync(values);
        }}
      />
      <MemoryFormDialog
        open={editTarget !== null}
        memory={editTarget}
        onClose={() => setEditTarget(null)}
        onSubmit={async (values) => {
          await editMut.mutateAsync({ id: editTarget!.memory_id, ...values });
        }}
      />
      <InvalidateMemoryDialog
        open={invalidateTarget !== null}
        memory={invalidateTarget}
        onClose={() => setInvalidateTarget(null)}
        onConfirm={async (m) => {
          await invalidateMut.mutateAsync(m);
        }}
      />
    </div>
  );
}
