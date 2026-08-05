import type { NamespaceConfigKey } from '../../types/api';

// Catalog 11 *_config registry (catalog — schema section locks 11 columns). Each
// entry maps a config column to its owning Feature, a human label, and an
// example JSON shape (used as a placeholder / "insert example" hint in the
// structured form). The UI treats every config as an opaque JSON blob — these
// examples are guidance only, never enforced validation.
//
// Tab assignment follows web UI design § 8.2 (5 tabs). The original § 8.2 table
// predates sparse_config + doc_summary_config (catalog→v1.0.8 added 4 of the
// 11); we slot them by Feature semantics: sparse_config → Retrieval (sparse
// retrieval), doc_summary_config → Processing (doc summary document summary index).

export type ConfigTab = 'retrieval' | 'processing' | 'memory' | 'advanced';

export interface ConfigMeta {
  key: NamespaceConfigKey;
  label: string;
  feature: string;
  tab: ConfigTab;
  example: Record<string, unknown>;
}

export const CONFIG_META: ConfigMeta[] = [
  {
    key: 'reranker_config',
    label: 'Reranker',
    feature: 'F02',
    tab: 'retrieval',
    example: { enabled: true, model: 'zerank-1-small', top_n: 20 },
  },
  {
    key: 'rag_fusion_config',
    label: 'RAG Fusion',
    feature: 'F36',
    tab: 'retrieval',
    example: { enabled: true, variant_count: 3 },
  },
  {
    key: 'crag_config',
    label: 'CRAG',
    feature: 'F37',
    tab: 'retrieval',
    example: {
      enabled: true,
      thresholds: { correct: 0.7, incorrect: 0.3 },
      multi_signal_features: ['top1', 'median', 'std', 'high_score_ratio', 'top1_top5_gap'],
      fallback: 'null',
    },
  },
  {
    key: 'complexity_config',
    label: 'Query Complexity',
    feature: 'F39',
    tab: 'retrieval',
    example: {
      enabled: true,
      labels: ['Simple', 'Complex', 'Chat'],
      routing_rules: { Simple: 'simple', Chat: 'chat', Complex: 'complex' },
      multi_turn_warning: true,
    },
  },
  {
    key: 'sparse_config',
    label: 'Sparse Retrieval',
    feature: 'F40',
    tab: 'retrieval',
    example: {
      enabled: true,
      topk: 100,
      max_tokens: 8192,
      cache_oom_fallback: 'skip',
      inverted_index_mmap: false,
    },
  },
  {
    key: 'parser_config',
    label: 'Parser',
    feature: 'F06',
    tab: 'processing',
    example: { strategy: 'layout-aware', ocr: false },
  },
  {
    key: 'enricher_config',
    label: 'Enricher',
    feature: 'F03',
    tab: 'processing',
    example: { enabled: false },
  },
  {
    key: 'cleaning_config',
    label: 'Cleaning',
    feature: 'F10',
    tab: 'processing',
    example: {},
  },
  {
    key: 'doc_summary_config',
    label: 'Document Summary Index',
    feature: 'F41',
    tab: 'processing',
    example: {
      enabled: true,
      summary_length: 512,
      llm_model: 'claude-haiku',
      fts5_fallback_enabled: true,
      regeneration_trigger: 'doc_update',
    },
  },
  {
    key: 'memory_config',
    label: 'Memory',
    feature: 'MEM01',
    tab: 'memory',
    example: { enabled: false },
  },
  {
    key: 'watcher_config',
    label: 'Watcher',
    feature: 'F21',
    tab: 'advanced',
    example: { enabled: false },
  },
];

export const CONFIG_KEYS: NamespaceConfigKey[] = CONFIG_META.map((c) => c.key);

export function configsForTab(tab: ConfigTab): ConfigMeta[] {
  return CONFIG_META.filter((c) => c.tab === tab);
}

export function metaFor(key: NamespaceConfigKey): ConfigMeta | undefined {
  return CONFIG_META.find((c) => c.key === key);
}
