import { useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import { ClockIcon } from '@heroicons/react/24/outline';
import type { OperationLogEntry, OperationLogFilter } from '../../types/api';
import { listOperations } from '../../api/operations';
import { parseAgentError } from '../../api/errors';
import { Input, Select, Button, Badge } from '../../components/ui';
import { LoadingSpinner } from '../../components/Common/LoadingSpinner';
import { ProgrammaticBanner } from '../../components/Common/ProgrammaticBanner';
import { ErrorDisplay } from '../../components/Common/ErrorDisplay';
import { DynamicColumns, type ColumnDef } from '../../components/Table/DynamicColumns';

// OperationLogPage (web UI design — CE simplified). Integrates the operation log
// CE operation_log via GET /api/v1/operations (business prefix — NOT
// /admin/operations, per V5-B4 P0-V5-66). Renders 8 CE columns; when the
// Filters: user_id / action / resource_type / date range.
//
// Date range: native <input type="date"> is used (no react-datepicker
// dependency added — keeps standalone deps minimal; only react-router-dom was
// approved for R3). It maps to from_timestamp / to_timestamp (Unix ms).

const PAGE_SIZE = 50;

// Distinct resource_type values surfaced as a filter (operation log enum).
const RESOURCE_TYPES = ['document', 'namespace', 'memory', 'query', 'user', 'db_connection', 'db_import'];

function fmtTimestamp(ms: number): string {
  const d = new Date(ms);
  return d.toISOString().replace('T', ' ').slice(0, 19) + 'Z';
}

function dateToMs(value: string, endOfDay: boolean): number | undefined {
  if (!value) return undefined;
  const d = new Date(value + (endOfDay ? 'T23:59:59.999Z' : 'T00:00:00.000Z'));
  return Number.isNaN(d.getTime()) ? undefined : d.getTime();
}

export function OperationLogPage() {
  const { t } = useTranslation();

  const [userId, setUserId] = useState('');
  const [action, setAction] = useState('');
  const [resourceType, setResourceType] = useState('all');
  const [fromDate, setFromDate] = useState('');
  const [toDate, setToDate] = useState('');
  const [offset, setOffset] = useState(0);

  const filter: OperationLogFilter = {
    user_id: userId || undefined,
    action: action || undefined,
    resource_type: resourceType === 'all' ? undefined : resourceType,
    from_timestamp: dateToMs(fromDate, false),
    to_timestamp: dateToMs(toDate, true),
    limit: PAGE_SIZE,
    offset,
  };

  const { data, isLoading, isError, error, refetch } = useQuery({
    queryKey: ['operations', filter],
    queryFn: () => listOperations(filter),
  });

  const ops = data?.operations ?? [];
  const total = data?.meta.total_count ?? 0;
  const hasNext = data?.meta.has_next ?? false;

  // CE 8 columns (web UI). `namespace` display maps to namespace_id;
  // `status` falls back to the schema default 'success'.
  const ceColumns: ColumnDef<OperationLogEntry>[] = useMemo(
    () => [
      {
        key: 'timestamp',
        header: t('admin.operations.colTimestamp'),
        className: 'whitespace-nowrap font-mono text-xs text-muted',
        render: (o) => fmtTimestamp(o.timestamp),
      },
      { key: 'user_id', header: t('admin.operations.colUser'), className: 'font-mono text-xs', render: (o) => o.user_id },
      {
        key: 'action',
        header: t('admin.operations.colAction'),
        render: (o) => <Badge variant="magma">{o.action}</Badge>,
      },
      { key: 'resource_type', header: t('admin.operations.colResourceType'), render: (o) => o.resource_type },
      {
        key: 'resource_id',
        header: t('admin.operations.colResourceId'),
        className: 'font-mono text-xs text-muted',
        render: (o) => o.resource_id ?? '—',
      },
      {
        key: 'namespace',
        header: t('admin.operations.colNamespace'),
        className: 'font-mono text-xs text-muted',
        render: (o) => o.namespace_id ?? '—',
      },
      {
        key: 'status',
        header: t('admin.operations.colStatus'),
        render: (o) => (
          <Badge variant={o.status === 'failed' || o.status === 'denied' ? 'error' : 'ok'}>
            {o.status ?? 'success'}
          </Badge>
        ),
      },
      {
        key: 'trace_id',
        header: t('admin.operations.colTraceId'),
        className: 'font-mono text-xs text-muted',
        render: (o) => o.trace_id ?? '—',
      },
    ],
    [t],
  );

  const columns = ceColumns;

  const resourceOptions = [
    { value: 'all', label: t('admin.operations.allResources') },
    ...RESOURCE_TYPES.map((r) => ({ value: r, label: r })),
  ];

  return (
    <div className="mx-auto max-w-7xl px-6 py-7 space-y-6">
      <div className="flex items-end justify-between gap-4">
        <div>
          <p className="mb-1.5 text-xs font-semibold uppercase tracking-[3px] text-magma-h">{t('admin.group')}</p>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">{t('admin.operations.title')}</h1>
          <p className="mt-1.5 text-sm text-muted">{t('admin.operations.subtitle')}</p>
        </div>
      </div>

      <ProgrammaticBanner
        comment={t('admin.operations.sdkComment')}
        snippet={
          <>
            <span className="text-magma-h">client</span>.<span className="text-amber">operations</span>.
            <span className="text-amber">list</span>(<span className="text-txt">resource_type</span>=
            <span className="text-ok">&quot;memory&quot;</span>)
          </>
        }
      />

      {/* Filter bar */}
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-5">
        <Input
          label={t('admin.operations.filterUserId')}
          value={userId}
          onChange={(e) => {
            setUserId(e.target.value);
            setOffset(0);
          }}
          placeholder="usr_…"
          data-testid="oplog-user-filter"
        />
        <Input
          label={t('admin.operations.filterAction')}
          value={action}
          onChange={(e) => {
            setAction(e.target.value);
            setOffset(0);
          }}
          placeholder="memory_create"
          data-testid="oplog-action-filter"
        />
        <Select
          label={t('admin.operations.filterResourceType')}
          value={resourceType}
          onChange={(v) => {
            setResourceType(v);
            setOffset(0);
          }}
          options={resourceOptions}
        />
        <Input
          type="date"
          label={t('admin.operations.filterFrom')}
          value={fromDate}
          onChange={(e) => {
            setFromDate(e.target.value);
            setOffset(0);
          }}
          data-testid="oplog-from-filter"
        />
        <Input
          type="date"
          label={t('admin.operations.filterTo')}
          value={toDate}
          onChange={(e) => {
            setToDate(e.target.value);
            setOffset(0);
          }}
          data-testid="oplog-to-filter"
        />
      </div>

      {/* Table */}
      <div className="overflow-hidden rounded-xl border border-line bg-surface card-shadow">
        <div className="flex items-center justify-between border-b border-line px-5 py-3.5">
          <h2 className="flex items-center gap-2 text-[15px] font-semibold text-txt">
            <ClockIcon className="h-4 w-4 text-magma-h" aria-hidden="true" />
            {t('admin.operations.recentActivity')}
          </h2>
          <span className="font-mono text-xs text-muted">{t('admin.operations.total', { count: total })}</span>
        </div>

        {isLoading ? (
          <div className="flex justify-center py-16">
            <LoadingSpinner />
          </div>
        ) : isError ? (
          <div className="p-5">
            <ErrorDisplay error={parseAgentError(error)} onRetry={() => void refetch()} />
          </div>
        ) : (
          <div className="overflow-x-auto px-1">
            <DynamicColumns<OperationLogEntry>
              columns={columns}
              rows={ops}
              rowKey={(o) => o.id}
              rowProps={(o) => ({ 'data-operation-id': o.id, 'data-action': o.action })}
              emptyLabel={t('admin.operations.empty')}
            />
          </div>
        )}

        {/* Pagination (offset-based) */}
        {!isLoading && !isError && (offset > 0 || hasNext) && (
          <div className="flex items-center justify-between border-t border-line px-5 py-3">
            <span className="text-xs text-muted">
              {t('admin.operations.showing', { from: offset + 1, to: offset + ops.length, total })}
            </span>
            <div className="flex gap-2">
              <Button
                size="sm"
                variant="secondary"
                disabled={offset <= 0}
                onClick={() => setOffset((o) => Math.max(0, o - PAGE_SIZE))}
              >
                {t('common.previous')}
              </Button>
              <Button size="sm" variant="secondary" disabled={!hasNext} onClick={() => setOffset((o) => o + PAGE_SIZE)}>
                {t('common.next')}
              </Button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
