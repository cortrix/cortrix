import type { ReactNode } from 'react';

// DynamicColumns (web UI design § 9-bis.2). A table whose column set is supplied
// at render time so the same component renders CE (8 cols) or Ent (12 cols)
// without a rewrite — OperationLogPage swaps the column list based on the
// `audit_log_ent` feature flag. Each column declares how to extract + render a
// cell from a row; unknown/missing values render as an em-dash.

export interface ColumnDef<Row> {
  /** Stable key (matches the backend field name where possible). */
  key: string;
  /** Header label (already translated by the caller). */
  header: ReactNode;
  /** Cell renderer; receives the full row. */
  render: (row: Row) => ReactNode;
  /** Tailwind alignment / width classes for the cell + header. */
  className?: string;
}

interface DynamicColumnsProps<Row> {
  columns: ColumnDef<Row>[];
  rows: Row[];
  rowKey: (row: Row) => string | number;
  /** Optional per-row data-* attributes for Agent-friendly DOM scraping. */
  rowProps?: (row: Row) => Record<string, string | number | undefined>;
  emptyLabel?: ReactNode;
}

export function DynamicColumns<Row>({
  columns,
  rows,
  rowKey,
  rowProps,
  emptyLabel,
}: DynamicColumnsProps<Row>) {
  return (
    <table className="w-full text-sm" data-testid="dynamic-columns-table">
      <thead>
        <tr className="border-b border-line text-left text-[11px] font-semibold uppercase tracking-wider text-muted">
          {columns.map((c) => (
            <th key={c.key} className={`px-4 py-2.5 ${c.className ?? ''}`}>
              {c.header}
            </th>
          ))}
        </tr>
      </thead>
      <tbody className="divide-y divide-line">
        {rows.length === 0 ? (
          <tr>
            <td colSpan={columns.length} className="py-14 text-center text-sm text-muted">
              {emptyLabel}
            </td>
          </tr>
        ) : (
          rows.map((row) => (
            <tr key={rowKey(row)} className="transition-colors hover:bg-magma/[0.05]" {...rowProps?.(row)}>
              {columns.map((c) => (
                <td key={c.key} className={`px-4 py-3 align-top ${c.className ?? ''}`}>
                  {c.render(row)}
                </td>
              ))}
            </tr>
          ))
        )}
      </tbody>
    </table>
  );
}
