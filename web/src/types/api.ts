// --- Document Upload ---

export interface UploadDocumentResponse {
  doc_id: number;
  content_hash: string;
  status: 'pending' | 'skipped' | 'updating';
  source_path: string;
  message: string;
}

export interface DocumentStatus {
  doc_id: number;
  source_type: string;
  source_path: string;
  status: 'pending' | 'processing' | 'ready' | 'error';
  error_message?: string;
  block_count: number;
  content_hash: string;
  mime_type: string;
  file_size: number;
  created_at: string;
  updated_at: string;
  processed_at?: string;
}

// --- Bulk document submit (TD-F42-BULK-SUBMIT — partial-success schema) ---
// POST /api/v1/documents/batch-submit accepts 1–100 documents and returns a
// partial-success envelope: `results[]` for accepted docs (Elasticsearch _bulk
// style) + `meta` with succeeded[] / failed[] (each failure carrying the
// GEN-Agent 5 fields) + coverage_ratio. BATCH-level failures (empty / size /
// duplicate) throw a top-level error instead.

export interface BatchSubmitDocument {
  doc_id: string;
  content?: string;
  url?: string;
  file_path?: string;
  metadata?: Record<string, unknown>;
}

export interface BatchSubmitRequest {
  namespace: string;
  documents: BatchSubmitDocument[];
  options?: {
    async?: boolean;
    on_duplicate?: 'skip' | 'overwrite' | 'error';
  };
}

export interface BatchSubmitResult {
  doc_id: string;
  task_id: string;
  status: 'submitted';
}

/** Per-doc failure — GEN-Agent 5 fields (TD-F42 § 2.3). */
export interface BatchSubmitFailure {
  doc_id: string;
  error_code: string;
  category: AgentErrorCategory;
  retryable: boolean;
  retry_after_ms: number | null;
  structured_data?: Record<string, unknown>;
}

export interface BatchSubmitResponse {
  results: BatchSubmitResult[];
  meta: {
    succeeded: string[];
    failed: BatchSubmitFailure[];
    coverage_ratio: number;
    total_submitted: number;
  };
}

// --- Search / Query ---

export interface QueryRequest {
  query: string;
  namespaces: string[];
  top_k?: number;
  timeout_ms?: number;
  // F04 §2.4 — the live cross-NS handler reads the singular `filter` key (a
  // JSONB pass-through). The plural `filters` was MVP-era drift that hit a dead
  // parser. Inner block_type[] is forwarded as-is; structured block_type
  // enforcement is F04 Phase-2 scope (line 60), not wired in the V1 executor.
  filter?: {
    block_type?: string[];
    source_path?: string;
    created_after?: string;
  };
  search_config?: {
    enable_vector?: boolean;
    enable_bm25?: boolean;
    enable_sql?: boolean;
  };
}

export interface QueryResponse {
  results: SearchResult[];
  sql_result?: SQLResult;
  meta: QueryMeta;
}

// F04 cross-NS ResultItem (cross_ns_response.cpp ResultItemToJson). child_id/
// parent_id are ULID strings; content is the matched child chunk, parent_content the
// enclosing parent block; namespace is which NS this hit came from (cross-NS).
export interface SearchResult {
  child_id: string;
  parent_id: string;
  content: string;
  parent_content: string;
  score: number;
  rerank_score: number;
  namespace: string;
  content_hash: string;
  metadata?: Record<string, string>;
}

export interface SQLResult {
  query: string;
  columns: string[];
  rows: unknown[][];
}

// F04 cross-NS query meta (cross_ns_response.cpp CrossNsMeta::ToJson). A-class data
// integrity: which NSs were queried/succeeded/failed + coverage.
export interface QueryNamespaceFailure {
  namespace: string;
  error_code: string;
  retryable: boolean;
  category: string;
  retry_after_ms?: number | null;
  message: string;
}

export interface QueryMeta {
  namespaces_queried: string[];
  namespaces_succeeded: string[];
  namespaces_failed: QueryNamespaceFailure[];
  coverage_ratio: number;
  latency_ms: number;
  deduplicated_chunks_count: number;
  warnings: string[];
}

// --- Namespace ---

export interface NamespaceListResponse {
  namespaces: NamespaceInfo[];
}

export interface NamespaceInfo {
  name: string;
  created_at: string | number;
  doc_count: number;
  block_count: number;
}

export interface CreateNamespaceRequest {
  name: string;
}

export interface CreateNamespaceResponse {
  name: string;
  created_at: string;
}

// --- System ---

export interface SystemStatus {
  version: string;
  uptime_seconds: number;
  namespace_count: number;
  total_doc_count: number;
  total_block_count: number;
  queue_status: {
    pending: number;
    processing: number;
  };
  llm_provider?: string;
  llm_enabled?: boolean;
}

// --- Feature flags ---
// Mirrors the backend GET /api/v1/system/features contract (P02a § 5.1):
// `edition` + a per-feature descriptor ({ enabled, placeholder, available_in }).

export type SystemEdition = string;

export interface FeatureDescriptor {
  enabled: boolean;
  placeholder: boolean;
  available_in: string;
}

export interface SystemFeaturesResponse {
  edition: SystemEdition;
  features: Record<string, FeatureDescriptor>;
}

export type FeatureFlags = SystemFeaturesResponse;

// --- LLM Config (cortrix-agent) ---

export interface LLMProvider {
  id: string;
  name: string;
  needs_key: boolean;
  needs_url: boolean;
  configured?: boolean;
  base_url?: string;
}

export interface ProviderConfigRequest {
  api_key?: string;
  base_url?: string;
}

export interface LLMModel {
  id: string;
  name: string;
  description: string;
  caps?: string[];
}

export interface LLMProvidersResponse {
  providers: LLMProvider[];
  models: Record<string, LLMModel[]>;
  current: {
    provider: string;
    model: string;
  };
}

export interface LLMRoleSelection {
  provider: string;
  model: string;
}

export interface LLMRolesResponse {
  providers: LLMProvider[];
  models: Record<string, LLMModel[]>;
  roles: {
    semantic_llm: LLMRoleSelection;
    vision_llm: LLMRoleSelection;
    agent_llm: LLMRoleSelection;
  };
}

export interface LLMConfigRequest {
  provider: string;
  model: string;
  api_key?: string;
  base_url?: string;
}

// --- GEN-Agent structured error (CLAUDE.md § 5 + AGENT_FRIENDLY.md) ---
// Standardised 5-field machine-readable error envelope returned by the backend
// for 4xx/5xx responses: { error: { code, message, retryable, category,
// retry_after_ms, structured_data } }. The ErrorDisplay component consumes this
// shape; non-conforming responses fall back to a plain message.

export type AgentErrorCategory =
  | 'auth'
  | 'quota'
  | 'transient'
  | 'permanent'
  | 'timeout';

export interface AgentError {
  code: string;
  message: string;
  retryable: boolean;
  category: AgentErrorCategory;
  retry_after_ms?: number | null;
  structured_data?: Record<string, unknown>;
}

export interface AgentErrorEnvelope {
  error: AgentError;
}

// --- Memory (MEM03 Memory Transparency) ---
// extraction_method 5-enum (P02a § 7.5 — Heroicons, no emoji).
export type ExtractionMethod =
  | 'llm_extracted'
  | 'user_created'
  | 'user_edit'
  | 'system'
  | 'imported';

export type MemoryType = 'fact' | 'preference' | 'event';

// MEM03 § 4.4 — status enum (active is the valid state; J5 aligns reader with
// MEM02 writer). invalidated = soft-deleted.
export type MemoryStatus = 'active' | 'tentative' | 'invalidated';

export interface MemoryItem {
  // A-class (8 fields, always returned)
  memory_id: string;
  content: string;
  memory_type: MemoryType;
  status: MemoryStatus;
  user_id: string;
  extracted_at: number;
  invalidated_at?: number | null;
  revoked_at?: number | null;

  // B-class (4 fields, only returned with ?explain=true)
  invalidated_by_block_id?: string | null;
  extraction_method?: ExtractionMethod | null;
  source_session_id?: string | null;
  source_interaction_id?: string | null;
}

export interface MemoryListMeta {
  total: number;
  page: number;
  page_size: number;
  namespaces_queried?: string[];
  coverage_ratio?: number;
  latency_ms?: number;
  warnings?: string[];
}

export interface MemoryListResponse {
  memories: MemoryItem[];
  meta: MemoryListMeta;
}

export interface MemoryListFilter {
  user_id: string;
  include_invalidated?: boolean;
  memory_type?: MemoryType;
  page?: number;
  page_size?: number;
  explain?: boolean;
}

export interface MemoryCreateRequest {
  user_id: string;
  content: string;
  memory_type: MemoryType;
}

export interface MemoryCreateResponse {
  memory_id: string;
  extracted_at: number;
  extraction_method: 'user_created';
}

export interface MemoryEditRequest {
  new_content?: string;
  new_memory_type?: MemoryType;
  expected_modified_at?: number;
}

export interface MemoryEditResponse {
  new_memory_id: string;
  old_memory_id: string;
}

export interface MemoryInvalidateResponse {
  memory_id: string;
  deleted_at: number;
}

// --- Namespace configs (F12 two-layer mapping, v1.0.8 — 11 *_config) ---
// Each of the 11 *_config columns is a free-form JSONB blob; per-feature schema
// is owned by F02/F03/F06/MEM01/F21/F10/F36/F37/F39/F40/F41 respectively. The
// UI treats them as opaque JSON objects (Hybrid Form + raw Monaco editor) and
// never hard-codes field validation beyond the documented examples.
export type NamespaceConfigKey =
  | 'reranker_config'
  | 'enricher_config'
  | 'parser_config'
  | 'memory_config'
  | 'watcher_config'
  | 'cleaning_config'
  | 'rag_fusion_config'
  | 'crag_config'
  | 'complexity_config'
  | 'sparse_config'
  | 'doc_summary_config';

export type NamespaceConfigs = Record<NamespaceConfigKey, Record<string, unknown>>;

export type NamespaceStatus = 'active' | 'deleted';
export type NamespaceIsolationMode = 'shared' | 'isolated';
export type NamespaceVisibility = 'tenant' | 'private' | 'public';

export interface NamespaceDetail extends NamespaceInfo {
  ns_id: string;
  description?: string;
  isolation_mode: NamespaceIsolationMode;
  visibility: NamespaceVisibility;
  status: NamespaceStatus;
  owner_user_id?: string | null;
  configs: NamespaceConfigs;
  // F05 admission state surfaced in the detail drawer (best-effort; optional).
  admission?: {
    estimated_size_bytes?: number;
    budget_limit_bytes?: number;
    quota_used?: number;
    quota_limit?: number;
  };
}

export interface CreateNamespaceFullRequest {
  name: string;
  description?: string;
  isolation_mode?: NamespaceIsolationMode;
  visibility?: NamespaceVisibility;
  configs?: Partial<NamespaceConfigs>;
}

export interface UpdateNamespaceRequest {
  description?: string;
  isolation_mode?: NamespaceIsolationMode;
  visibility?: NamespaceVisibility;
  configs?: Partial<NamespaceConfigs>;
}

// --- Auth (P08 — HttpOnly cookie model, P02a § 9.2) ---
// The auth token lives in an HttpOnly cookie managed by the backend; the store
// only tracks UI state. `currentUser` is populated by GET /api/v1/auth/me.

export type UserRole = 'admin' | 'user';
export type UserStatus = 'active' | 'disabled';

export interface CurrentUser {
  id: string;
  email: string;
  display_name?: string;
  role: UserRole;
}

/** GET /api/v1/auth/me — current session probe (P08 § 2.9). */
export interface AuthMeResponse {
  id: string;
  email: string;
  display_name?: string;
  email_verified?: boolean;
  role?: UserRole;
  edition?: string;
}

/** POST /api/v1/auth/login — Web UI path (P08 § 2.3): Set-Cookie + user only. */
export interface LoginResponse {
  user: AuthMeResponse;
  expires_in?: number;
}

/** POST /api/v1/admin/bootstrap — programmatic admin key (P08 § 2.13.2.b). */
export interface BootstrapResponse {
  admin_api_key: string;
  expires_at: string | null;
  scopes: string[];
}

// --- API Keys (P08 § 2.13.3 — user-level keys for SDK / MCP) ---

export type ApiKeyStatus = 'active' | 'revoked' | 'expired';

/** GET /api/v1/auth/api-keys — list item (no plaintext key). */
export interface ApiKeySummary {
  id: string;
  name: string;
  key_prefix: string;
  created_at: string;
  last_used_at?: string | null;
  expires_at?: string | null;
  status: ApiKeyStatus;
}

export interface ApiKeyCreateRequest {
  name: string;
  expires_at?: string | null;
}

/** POST /api/v1/auth/api-keys — plaintext `key` returned ONCE on create. */
export interface ApiKeyCreateResponse {
  id: string;
  key: string;
  name: string;
  created_at: string;
  expires_at?: string | null;
}

// --- Admin: Users (P08 § 2.13-bis — 5 endpoints) ---

export interface UserRecord {
  id: string;
  email: string;
  display_name?: string;
  role: UserRole;
  status: UserStatus;
  email_verified?: boolean;
  created_at?: string;
}

export interface UserListFilter {
  q?: string;
  status?: UserStatus;
  role?: UserRole;
  page?: number;
  limit?: number;
}

export interface UserListResponse {
  users: UserRecord[];
  total: number;
  page: number;
  limit: number;
}

export interface UserCreateRequest {
  email: string;
  password: string;
  role: UserRole;
  display_name?: string;
}

export interface UserUpdateRequest {
  email?: string;
  display_name?: string;
  role?: UserRole;
  email_verified?: boolean;
}

export interface UserMutationResponse {
  user: UserRecord;
}

// --- Operation Log (F18a CE — GET /api/v1/operations) ---
// Response shape is the SoT (F18a § 6.1). The §9-bis.2 table maps the display
// column `namespace` -> `namespace_id` and `status` -> the schema default
// ('success'); Ent (F18b) adds source_ip / user_agent / details_json /
// failure_reason via the audit_log_extension table (surfaced when the
// `audit_log_ent` flag is on — DynamicColumns swaps the column set).
export type OperationStatus = 'success' | 'failed' | 'denied';

export interface OperationLogEntry {
  id: number;
  timestamp: number;
  user_id: string;
  action: string;
  namespace_id?: string | null;
  resource_type: string;
  resource_id?: string | null;
  summary?: string | null;
  status?: OperationStatus;
  trace_id?: string | null;
  session_id?: string | null;
  // Extended audit columns (F18b — only present on extended deployments).
  source_ip?: string | null;
  user_agent?: string | null;
  details_json?: Record<string, unknown> | null;
  failure_reason?: string | null;
}

export interface OperationLogFilter {
  user_id?: string;
  action?: string;
  resource_type?: string;
  trace_id?: string;
  from_timestamp?: number;
  to_timestamp?: number;
  limit?: number;
  offset?: number;
}

export interface OperationLogResponse {
  operations: OperationLogEntry[];
  meta: {
    total_count: number;
    has_next: boolean;
    next_offset?: number;
  };
}

// --- Chat ---

export interface ChatRequest {
  message: string;
  namespace: string;
  conversation_id?: string;
  top_k?: number;
}

export type ChatEvent =
  | { type: 'token'; data: { text: string } }
  | { type: 'sources'; data: { sources: SearchResult[] } }
  | { type: 'done'; data: { conversation_id: string } }
  | { type: 'error'; data: { message: string } };
