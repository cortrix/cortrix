import { get } from './client';

// NOTE: The standalone Connector page (S3 pull / directory monitor UI) was
// removed in P02a-R1-S1 (F17 -> Phase 2). Only the watcher listing used by the
// Upload / DocumentList tree view is retained here. The backend route
// `/api/v1/connector/watchers` stays the source of watch-directory groupings.

export interface WatcherStats {
  total_files: number;
  imported: number;
  updated: number;
  skipped_unchanged: number;
  skipped_filtered: number;
  skipped_error: number;
  deleted: number;
  elapsed_s: number;
}

export interface WatcherEntry {
  id: string;
  data_dir: string;
  namespace_name: string;
  watching: boolean;
  status: 'watching' | 'stopped';
  stats: WatcherStats;
}

/** List watch-directory watchers (used by Upload DocumentList to group docs by root). */
export async function listWatchers(): Promise<WatcherEntry[]> {
  const res = await get<{ watchers: WatcherEntry[] }>('/api/v1/connector/watchers');
  return res.watchers ?? [];
}
