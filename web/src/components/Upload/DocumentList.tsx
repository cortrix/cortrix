import { useState, useEffect } from 'react';
import { usePolling } from '../../hooks/usePolling';
import {
  DocumentTextIcon, DocumentIcon, PhotoIcon, TableCellsIcon, TrashIcon, ArrowPathIcon,
  CircleStackIcon, CheckCircleIcon, ExclamationCircleIcon, ClockIcon, CpuChipIcon,
  FolderIcon, FolderOpenIcon, ChevronRightIcon, ChevronDownIcon, ArrowUpTrayIcon,
} from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useUploadStore } from '../../store/useUploadStore';
import { useAppStore } from '../../store/useAppStore';
import { listWatchers } from '../../api/connector';
import type { WatcherEntry } from '../../api/connector';
import { formatFileSize } from '../../utils/formatters';
import type { DocumentStatus } from '../../types/api';

// ── Status config ────────────────────────────────────────────────────────────

const statusConfig: Record<string, { icon: typeof CheckCircleIcon; color: string }> = {
  ready:      { icon: CheckCircleIcon, color: 'text-ok' },
  processing: { icon: CpuChipIcon,          color: 'text-magma-h' },
  pending:    { icon: ClockIcon,        color: 'text-amber' },
  error:      { icon: ExclamationCircleIcon,  color: 'text-red-500' },
  stale:      { icon: ExclamationCircleIcon,  color: 'text-orange-500' },
};

function getFileIcon(doc: DocumentStatus) {
  const ext = doc.source_path.split('.').pop()?.toLowerCase() || '';
  const mime = doc.mime_type || '';
  if (mime.startsWith('image/') || ['png', 'jpg', 'jpeg', 'gif', 'webp'].includes(ext)) return PhotoIcon;
  if (['csv', 'xlsx', 'xls'].includes(ext)) return TableCellsIcon;
  if (ext === 'pdf' || mime === 'application/pdf') return DocumentIcon;
  return DocumentTextIcon;
}

function formatDate(val: string | number): string {
  const d = typeof val === 'number' ? new Date(val * 1000) : new Date(val);
  return d.toLocaleDateString() + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

// ── Tree data structures ─────────────────────────────────────────────────────

interface DirNode {
  type: 'dir';
  name: string;
  fullPath: string;
  depth: number;
  children: TreeNode[];
}

interface FileNode {
  type: 'file';
  name: string;
  doc: DocumentStatus;
  depth: number;
}

type TreeNode = DirNode | FileNode;

/** Longest common directory prefix across a list of absolute paths. */
function commonPathPrefix(paths: string[]): string {
  if (paths.length === 0) return '';
  const splitPath = (p: string) => p.replace(/\\/g, '/').split('/');
  const allParts = paths.map(splitPath);
  const ref = allParts[0];
  let depth = 0;
  outer: for (let i = 0; i < ref.length - 1; i++) {
    for (const parts of allParts) {
      if (parts[i] !== ref[i]) break outer;
    }
    depth = i + 1;
  }
  return ref.slice(0, depth).join('/');
}

/** Build a directory tree from a list of documents relative to a known prefix. */
function buildTree(docs: DocumentStatus[], prefix: string): DirNode {
  interface MDir {
    name: string;
    fullPath: string;
    subdirs: Map<string, MDir>;
    files: DocumentStatus[];
  }

  const rootName = prefix.split('/').filter(Boolean).pop() || prefix || '/';
  const root: MDir = { name: rootName, fullPath: prefix, subdirs: new Map(), files: [] };

  for (const doc of docs) {
    const normalized = doc.source_path.replace(/\\/g, '/');
    const relative = normalized.startsWith(prefix)
      ? normalized.slice(prefix.length).replace(/^\//, '')
      : normalized;

    const parts = relative.split('/');
    parts.pop(); // strip filename — dirs only below

    let cur = root;
    for (const part of parts) {
      if (!part) continue;
      if (!cur.subdirs.has(part)) {
        cur.subdirs.set(part, {
          name: part,
          fullPath: cur.fullPath + '/' + part,
          subdirs: new Map(),
          files: [],
        });
      }
      cur = cur.subdirs.get(part)!;
    }
    cur.files.push(doc);
  }

  // Convert MDir → DirNode recursively, sorting dirs first then files
  const convert = (mdir: MDir, depth: number): DirNode => {
    const children: TreeNode[] = [];
    [...mdir.subdirs.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .forEach(([, sub]) => children.push(convert(sub, depth + 1)));
    [...mdir.files]
      .sort((a, b) => a.source_path.localeCompare(b.source_path))
      .forEach(doc => {
        const name = doc.source_path.replace(/\\/g, '/').split('/').pop() || doc.source_path;
        children.push({ type: 'file', name, doc, depth: depth + 1 });
      });
    return { type: 'dir', name: mdir.name, fullPath: mdir.fullPath, depth, children };
  };

  return convert(root, 0);
}

/** Flatten tree to a renderable list respecting expand/collapse state. */
function flattenTree(node: TreeNode, expanded: Set<string>): TreeNode[] {
  const result: TreeNode[] = [node];
  if (node.type === 'dir' && expanded.has(node.fullPath)) {
    for (const child of node.children) {
      result.push(...flattenTree(child, expanded));
    }
  }
  return result;
}

// ── File row (shared between tree and flat list) ─────────────────────────────

function FileRow({
  node,
  onDelete,
  confirmId,
  onConfirm,
  onCancelConfirm,
}: {
  node: FileNode;
  onDelete: (id: number) => void;
  confirmId: number | null;
  onConfirm: (id: number) => void;
  onCancelConfirm: () => void;
}) {
  const { t } = useTranslation();
  const doc = node.doc;
  const Icon = getFileIcon(doc);
  const sCfg = statusConfig[doc.status] || statusConfig.pending;
  const StatusIcon = sCfg.icon;
  const isConfirming = confirmId === doc.doc_id;
  const indent = node.depth * 16;

  return (
    <div
      className="grid grid-cols-[1fr_80px_70px_80px_120px_36px] gap-1 px-3 py-2.5 items-center
                 border-b last:border-b-0 border-line
                 hover:bg-surface2/50 transition-colors"
    >
      {/* Name */}
      <div className="flex items-center gap-2 min-w-0" style={{ paddingLeft: indent }}>
        <div className="w-6 h-6 rounded flex items-center justify-center shrink-0 bg-surface2">
          <Icon className="w-3.5 h-3.5 text-muted" />
        </div>
        <div className="min-w-0">
          <p className="text-xs font-medium text-txt truncate" title={doc.source_path}>
            {node.name}
          </p>
          <p className="text-[10px] text-muted">
            #{doc.doc_id} · {doc.mime_type || 'unknown'}
          </p>
        </div>
      </div>

      {/* Status */}
      <div className="flex justify-center">
        <div className="flex items-center gap-1" title={doc.status}>
          <StatusIcon className={`w-3 h-3 ${sCfg.color}`} />
          <span className={`text-[10px] ${sCfg.color}`}>{doc.status}</span>
        </div>
      </div>

      {/* Blocks */}
      <p className="text-[11px] text-muted text-right tabular-nums">{doc.block_count}</p>

      {/* Size */}
      <p className="text-[11px] text-muted text-right tabular-nums">
        {doc.file_size > 0 ? formatFileSize(doc.file_size) : '—'}
      </p>

      {/* Date */}
      <p className="text-[10px] text-muted text-right tabular-nums">
        {formatDate(doc.created_at)}
      </p>

      {/* Delete */}
      <div className="flex justify-center">
        {isConfirming ? (
          <button
            onClick={() => { onDelete(doc.doc_id); onCancelConfirm(); }}
            className="p-0.5 rounded bg-red-500 text-white hover:bg-red-600 text-[10px] font-medium"
            title={t('documents.deleteConfirm')}
          >✓</button>
        ) : (
          <button
            onClick={() => onConfirm(doc.doc_id)}
            className="p-1 rounded-lg hover:bg-red-50 text-muted hover:text-red-500 dark:hover:bg-red-900/20 dark:hover:text-red-400 transition-colors"
          >
            <TrashIcon className="w-3 h-3" />
          </button>
        )}
      </div>
    </div>
  );
}

// ── Dir row ───────────────────────────────────────────────────────────────────

function DirRow({
  node,
  expanded,
  onToggle,
}: {
  node: DirNode;
  expanded: boolean;
  onToggle: () => void;
}) {
  const fileCount = countFiles(node);
  const indent = node.depth * 16;
  const FolderGlyph = expanded ? FolderOpenIcon : FolderIcon;
  const ChevronGlyph = expanded ? ChevronDownIcon : ChevronRightIcon;

  return (
    <div
      className="flex items-center gap-2 px-3 py-2 cursor-pointer select-none
                 border-b border-line
                 hover:bg-surface2/60 transition-colors"
      style={{ paddingLeft: 12 + indent }}
      onClick={onToggle}
      title={node.fullPath}
    >
      <ChevronGlyph className="w-3.5 h-3.5 text-muted shrink-0" />
      <FolderGlyph className="w-4 h-4 text-cortrix-500 dark:text-cortrix-400 shrink-0" />
      <span className="text-xs font-semibold text-txt truncate flex-1">
        {node.name}
      </span>
      <span className="text-[10px] text-muted shrink-0">
        {fileCount} files
      </span>
    </div>
  );
}

function countFiles(node: DirNode): number {
  let count = 0;
  for (const child of node.children) {
    if (child.type === 'file') count++;
    else count += countFiles(child);
  }
  return count;
}

// ── WatchDirectoriesView ──────────────────────────────────────────────────────

/**
 * Renders ALL watcher groups as a single unified tree table.
 * Each watcher root (data_dir) appears as a top-level collapsible folder.
 * This avoids the visual fragmentation of one box per watcher.
 */
function WatchDirectoriesView({
  groups,
  unmatched,
}: {
  groups: { watcher: WatcherEntry; docs: DocumentStatus[] }[];
  unmatched: DocumentStatus[];
}) {
  const { deleteDoc } = useUploadStore();

  // Build one DirNode root per watcher group
  const allRoots: DirNode[] = [
    ...groups.map(({ watcher, docs }) => buildTree(docs, watcher.data_dir)),
    ...(unmatched.length > 0
      ? [buildTree(unmatched, commonPathPrefix(unmatched.map(d => d.source_path)))]
      : []),
  ];

  // Auto-expand all roots and their immediate children
  const initialExpanded = new Set<string>();
  for (const root of allRoots) {
    initialExpanded.add(root.fullPath);
    for (const child of root.children) {
      if (child.type === 'dir') initialExpanded.add(child.fullPath);
    }
  }

  const [expanded, setExpanded] = useState<Set<string>>(initialExpanded);
  const [confirmId, setConfirmId] = useState<number | null>(null);

  const toggle = (path: string) =>
    setExpanded(prev => {
      const next = new Set(prev);
      next.has(path) ? next.delete(path) : next.add(path);
      return next;
    });

  // Flatten all roots into one renderable list
  const flat: TreeNode[] = allRoots.flatMap(root => flattenTree(root, expanded));
  const totalFiles = allRoots.reduce((sum, root) => sum + countFiles(root), 0);

  return (
    <div className="mt-4">
      <div className="flex items-center gap-2 mb-2">
        <FolderIcon className="w-4 h-4 text-cortrix-500 dark:text-cortrix-400" />
        <span className="text-sm font-semibold text-txt">Watch Directories</span>
        <span className="text-xs text-muted">({totalFiles} files)</span>
      </div>

      <div className="rounded-xl border border-line overflow-hidden">
        {/* Shared column header */}
        <div className="grid grid-cols-[1fr_80px_70px_80px_120px_36px] gap-1 px-3 py-2 text-[10px] font-semibold uppercase tracking-wider text-muted bg-bgalt border-b border-line">
          <span>Name</span>
          <span className="text-center">Status</span>
          <span className="text-right">Blocks</span>
          <span className="text-right">Size</span>
          <span className="text-right">Date</span>
          <span />
        </div>

        {flat.map((node, i) =>
          node.type === 'dir' ? (
            <DirRow
              key={`dir-${node.fullPath}-${i}`}
              node={node}
              expanded={expanded.has(node.fullPath)}
              onToggle={() => toggle(node.fullPath)}
            />
          ) : (
            <FileRow
              key={`file-${node.doc.doc_id}`}
              node={node}
              onDelete={deleteDoc}
              confirmId={confirmId}
              onConfirm={setConfirmId}
              onCancelConfirm={() => setConfirmId(null)}
            />
          )
        )}
      </div>
    </div>
  );
}

// ── FlatList (for uploaded files without a meaningful directory structure) ────

function FlatList({ docs }: { docs: DocumentStatus[] }) {
  const { t } = useTranslation();
  const { deleteDoc } = useUploadStore();
  const [confirmId, setConfirmId] = useState<number | null>(null);

  return (
    <div className="rounded-xl border border-line overflow-hidden">
      <div className="grid grid-cols-[1fr_80px_70px_80px_120px_36px] gap-1 px-3 py-2 text-[10px] font-semibold uppercase tracking-wider text-muted bg-bgalt border-b border-line">
        <span>{t('kb.colName')}</span>
        <span className="text-center">{t('kb.colStatus')}</span>
        <span className="text-right">{t('kb.colBlocks')}</span>
        <span className="text-right">{t('kb.colSize')}</span>
        <span className="text-right">{t('kb.colDate')}</span>
        <span />
      </div>
      {docs.map(doc => {
        const name = doc.source_path.replace(/\\/g, '/').split('/').pop() || doc.source_path;
        return (
          <FileRow
            key={`upload-${doc.doc_id}`}
            node={{ type: 'file', name, doc, depth: 0 }}
            onDelete={deleteDoc}
            confirmId={confirmId}
            onConfirm={setConfirmId}
            onCancelConfirm={() => setConfirmId(null)}
          />
        );
      })}
    </div>
  );
}

// ── DocumentList (main export) ────────────────────────────────────────────────

export function DocumentList() {
  const { documents, documentsLoading, loadDocuments } = useUploadStore();
  const currentNamespace = useAppStore((s) => s.currentNamespace);
  const { t } = useTranslation();

  // Fetch watchers for the current namespace to group tree views by watcher root
  const [nsWatchers, setNsWatchers] = useState<WatcherEntry[]>([]);
  useEffect(() => {
    listWatchers()
      .then(list => setNsWatchers(list.filter(w => w.namespace_name === currentNamespace)))
      .catch(() => setNsWatchers([]));
  }, [currentNamespace]);

  const realDocs = documents.filter(d => d.source_type !== 'memory_session');
  const watchDocs = realDocs.filter(d => d.source_type === 'watch_dir');
  const uploadDocs = realDocs.filter(d => d.source_type !== 'watch_dir');

  // Auto-poll while any document is pending or processing
  const hasActive = realDocs.some(d => d.status === 'pending' || d.status === 'processing');
  usePolling(loadDocuments, 3000, hasActive);
  const readyCount = realDocs.filter(d => d.status === 'ready').length;
  const totalBlocks = realDocs.reduce((sum, d) => sum + d.block_count, 0);

  // Group watch_dir docs by their watcher root (data_dir).
  // This prevents multiple watchers sharing a common ancestor from merging
  // into one tree due to commonPathPrefix collapsing the root.
  const watcherGroups: { watcher: WatcherEntry; docs: DocumentStatus[] }[] = [];
  const unmatchedWatchDocs: DocumentStatus[] = [];

  for (const doc of watchDocs) {
    const normalized = doc.source_path.replace(/\\/g, '/');
    const watcher = nsWatchers.find(w => {
      const dir = w.data_dir.replace(/\\/g, '/');
      return normalized.startsWith(dir === '/' ? dir : dir + '/') || normalized === dir;
    });
    if (watcher) {
      const group = watcherGroups.find(g => g.watcher.id === watcher.id);
      if (group) group.docs.push(doc);
      else watcherGroups.push({ watcher, docs: [doc] });
    } else {
      unmatchedWatchDocs.push(doc);
    }
  }
  // Sort by directory name for stable ordering
  watcherGroups.sort((a, b) => a.watcher.data_dir.localeCompare(b.watcher.data_dir));

  return (
    <div className="mt-10">
      {/* Header */}
      <div className="flex items-center justify-between mb-1">
        <div className="flex items-center gap-3">
          <div className="w-8 h-8 rounded-lg flex items-center justify-center bg-cortrix-100 dark:bg-cortrix-800/20">
            <CircleStackIcon className="w-4.5 h-4.5 text-cortrix-600 dark:text-cortrix-400" />
          </div>
          <div>
            <h2 className="text-lg font-bold text-txt">{t('kb.title')}</h2>
            <p className="text-xs text-muted">
              {t('kb.stats', { docs: realDocs.length, ready: readyCount, blocks: totalBlocks })}
            </p>
          </div>
        </div>
        <button
          onClick={loadDocuments}
          disabled={documentsLoading}
          className="flex items-center gap-1.5 px-3 py-1.5 text-xs rounded-lg border border-line hover:bg-surface2 text-muted disabled:opacity-50"
        >
          <ArrowPathIcon className={`w-3.5 h-3.5 ${documentsLoading ? 'animate-spin' : ''}`} />
          {t('kb.refresh')}
        </button>
      </div>

      {/* Loading */}
      {documentsLoading && realDocs.length === 0 && (
        <div className="text-center py-12 text-muted">
          <ArrowPathIcon className="w-6 h-6 mx-auto mb-2 animate-spin" />
          <p className="text-sm">{t('common.loading')}</p>
        </div>
      )}

      {/* Empty */}
      {!documentsLoading && realDocs.length === 0 && (
        <div className="text-center py-16 text-muted border border-dashed border-line rounded-xl mt-4">
          <CircleStackIcon className="w-10 h-10 mx-auto mb-3 opacity-30" />
          <p className="text-sm font-medium">{t('kb.empty')}</p>
          <p className="text-xs mt-1">{t('kb.emptyHint')}</p>
        </div>
      )}

      {/* All watch directories merged into one unified tree table */}
      {(watcherGroups.length > 0 || unmatchedWatchDocs.length > 0) && (
        <WatchDirectoriesView groups={watcherGroups} unmatched={unmatchedWatchDocs} />
      )}

      {/* Uploaded Files (flat) */}
      {uploadDocs.length > 0 && (
        <div className="mt-4">
          <div className="flex items-center gap-2 mb-2">
            <ArrowUpTrayIcon className="w-4 h-4 text-muted" />
            <span className="text-sm font-semibold text-txt">
              Uploaded Files
            </span>
            <span className="text-xs text-muted">({uploadDocs.length} files)</span>
          </div>
          <FlatList docs={uploadDocs} />
        </div>
      )}
    </div>
  );
}
