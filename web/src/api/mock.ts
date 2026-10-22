import type {
  UploadDocumentResponse,
  DocumentStatus,
  QueryResponse,
  NamespaceInfo,
  SystemStatus,
  FeatureFlags,
  SearchResult,
  MemoryItem,
  MemoryListFilter,
  MemoryListResponse,
  MemoryCreateRequest,
  MemoryCreateResponse,
  MemoryEditRequest,
  MemoryEditResponse,
  MemoryInvalidateResponse,
  NamespaceConfigs,
  NamespaceDetail,
  CreateNamespaceFullRequest,
  UpdateNamespaceRequest,
  AuthMeResponse,
  BootstrapResponse,
  UserRecord,
  UserStatus,
  UserListFilter,
  UserListResponse,
  UserCreateRequest,
  UserUpdateRequest,
  UserMutationResponse,
  OperationLogEntry,
  OperationLogFilter,
  OperationLogResponse,
  ApiKeySummary,
  ApiKeyCreateRequest,
  ApiKeyCreateResponse,
  BatchSubmitRequest,
  BatchSubmitResponse,
  BatchSubmitResult,
  BatchSubmitFailure,
} from '../types/api';
import type { LiveResponse, ReadyResponse } from './health';
import { ApiError } from './client';

// Simulate network delay
const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

let nextDocId = 100;

export const mockApi = {
  async uploadDocument(_ns: string, _file: File): Promise<UploadDocumentResponse> {
    await delay(300);
    const id = nextDocId++;
    return {
      doc_id: id,
      content_hash: `sha256:${Math.random().toString(36).slice(2, 18)}`,
      status: 'pending',
      source_path: _file.name,
      message: 'Document accepted for processing',
    };
  },

  async getDocumentStatus(_ns: string, docId: number): Promise<DocumentStatus> {
    await delay(200);
    // Simulate progression: pending → processing → ready
    const roll = Math.random();
    const status = roll > 0.7 ? 'ready' : roll > 0.3 ? 'processing' : 'pending';
    return {
      doc_id: docId,
      source_type: 'file',
      source_path: 'uploaded-document.pdf',
      status,
      block_count: status === 'ready' ? Math.floor(Math.random() * 30) + 5 : 0,
      content_hash: 'sha256:mock',
      mime_type: 'application/pdf',
      file_size: 2400000,
      created_at: new Date().toISOString(),
      updated_at: new Date().toISOString(),
      processed_at: status === 'ready' ? new Date().toISOString() : undefined,
    };
  },

  async search(ns: string, query: string): Promise<QueryResponse> {
    await delay(400);
    const results: SearchResult[] = [
      {
        child_id: '01JBLK1042CHILD0000000000',
        parent_id: '01JBLK1042PARENT000000000',
        content: `The core **architectural decisions** for the **semantic storage** engine include: (1) SQLite as the metadata store with WAL mode, (2) hnswlib for ANN search with 1024-dim vectors, (3) RRF fusion combining vector similarity and BM25 for "${query}"...`,
        parent_content: '',
        score: 0.923,
        rerank_score: 0.91,
        namespace: ns,
        content_hash: 'sha256:mock1042',
      },
      {
        child_id: '01JBLK1521CHILD0000000000',
        parent_id: '01JBLK1521PARENT000000000',
        content: `Row from \`products\` table: "Cortrix Cloud", category "semantic_storage", includes architecture decisions reference matching "${query}"...`,
        parent_content: '',
        score: 0.842,
        rerank_score: 0.83,
        namespace: ns,
        content_hash: 'sha256:mock1521',
      },
      {
        child_id: '01JBLK1680CHILD0000000000',
        parent_id: '01JBLK1680PARENT000000000',
        content: `User asked about **storage architecture** trade-offs. AI compared SQLite vs PostgreSQL for metadata, noting SQLite's zero-deployment advantage for MVP...`,
        parent_content: '',
        score: 0.781,
        rerank_score: 0.77,
        namespace: ns,
        content_hash: 'sha256:mock1680',
      },
    ];
    return {
      results,
      meta: {
        namespaces_queried: [ns],
        namespaces_succeeded: [ns],
        namespaces_failed: [],
        coverage_ratio: 1.0,
        latency_ms: 42,
        deduplicated_chunks_count: 0,
        warnings: [],
      },
    };
  },

  async getNamespaces(): Promise<NamespaceInfo[]> {
    await delay(150);
    return [
      { name: 'default', created_at: '2026-01-15T00:00:00Z', doc_count: 47, block_count: 1283 },
      { name: 'legal-docs', created_at: '2026-02-01T00:00:00Z', doc_count: 12, block_count: 340 },
    ];
  },

  async createNamespace(name: string): Promise<{ name: string; created_at: string }> {
    await delay(200);
    return { name, created_at: new Date().toISOString() };
  },

  async deleteNamespace(_name: string): Promise<void> {
    await delay(200);
  },

  async getSystemStatus(): Promise<SystemStatus> {
    await delay(100);
    return {
      version: '1.0.0-rc.2',
      uptime_seconds: 3600 + Math.floor(Math.random() * 100),
      namespace_count: 2,
      total_doc_count: 47,
      total_block_count: 1283,
      queue_status: { pending: 2, processing: 1 },
      llm_provider: 'GLM-4',
      llm_enabled: true,
    };
  },

  async getFeatureFlags(): Promise<FeatureFlags> {
    await delay(50);
    // CE default: all three Ent features are placeholders, none enabled (§ 5.1).
    return {
      edition: 'ce',
      features: {
        text_to_sql: { enabled: false, placeholder: true, available_in: 'V1.5' },
        audit_log_ent: { enabled: false, placeholder: true, available_in: 'V1.5' },
        multi_tenant_switch: { enabled: false, placeholder: true, available_in: 'Cloud V1.5' },
      },
    };
  },

  // ─── Health probes (F20-7 — /live + /ready) ───────────────────────────────
  async getLive(): Promise<LiveResponse> {
    await delay(60);
    return { status: 'alive', uptime_seconds: 4200 + Math.floor(Math.random() * 100), version: '1.0.0-rc.2' };
  },

  async getReady(): Promise<ReadyResponse> {
    await delay(90);
    // Standalone demo: all 5 components ready (F20-7 § 8.4 component set).
    return {
      status: 'ready',
      uptime_seconds: 4200,
      components: {
        catalog: { status: 'ok', bloom_filter_ready: true },
        vector_index: { status: 'ok', wal_lag_bytes: 0, model_loaded: true },
        secret_provider: { status: 'ok', provider_type: 'env' },
        spc_pipeline: { status: 'ok', queue_depth: 3 },
        memory_store: { status: 'ok' },
      },
      warnings: [],
    };
  },

  // ─── Bulk document submit (TD-F42-BULK-SUBMIT — partial-success schema) ────
  async batchSubmit(req: BatchSubmitRequest): Promise<BatchSubmitResponse> {
    await delay(420);
    const docs = req.documents ?? [];
    if (docs.length === 0) {
      throw new ApiError(
        400,
        JSON.stringify({
          error: {
            code: 'CX_ERR_BATCH_EMPTY',
            message: 'documents must not be empty',
            retryable: false,
            category: 'permanent',
          },
        }),
      );
    }
    if (docs.length > 100) {
      throw new ApiError(
        400,
        JSON.stringify({
          error: {
            code: 'CX_ERR_BATCH_SIZE_EXCEEDED',
            message: 'A batch may contain at most 100 documents',
            retryable: false,
            category: 'permanent',
            structured_data: { limit: 100, received: docs.length },
          },
        }),
      );
    }
    // Demo partial-success: every 4th document fails (alternating quota /
    // transient) so the success/failure grouping UI is exercised.
    const results: BatchSubmitResult[] = [];
    const succeeded: string[] = [];
    const failed: BatchSubmitFailure[] = [];
    docs.forEach((d, i) => {
      const failThis = (i + 1) % 4 === 0;
      if (!failThis) {
        results.push({ doc_id: d.doc_id, task_id: `task_${(nextDocId++).toString(16)}`, status: 'submitted' });
        succeeded.push(d.doc_id);
      } else if (i % 8 === 3) {
        failed.push({
          doc_id: d.doc_id,
          error_code: 'CX_ERR_NS_QUOTA_EXCEEDED',
          category: 'quota',
          retryable: false,
          retry_after_ms: null,
          structured_data: { ns_quota_used: 1000, ns_quota_limit: 1000 },
        });
      } else {
        failed.push({
          doc_id: d.doc_id,
          error_code: 'CX_ERR_TRANSIENT_LLM_RATE_LIMIT',
          category: 'transient',
          retryable: true,
          retry_after_ms: 5000,
          structured_data: { rate_limit_remaining: 0 },
        });
      }
    });
    return {
      results,
      meta: {
        succeeded,
        failed,
        coverage_ratio: docs.length ? succeeded.length / docs.length : 0,
        total_submitted: docs.length,
      },
    };
  },

  // Chat SSE simulation
  streamChat(
    _ns: string,
    _message: string,
    onToken: (text: string) => void,
    onSources: (sources: SearchResult[]) => void,
    onDone: (conversationId: string) => void,
    onError: (msg: string) => void,
  ): () => void {
    const tokens = [
      '**Reciprocal Rank Fusion (RRF)** ',
      'combines vector search ',
      'and BM25 results:\n\n',
      '```\nrrf_score = Σ 1 / (k + rank_i)  where k = 60\n```\n\n',
      'The query pipeline executes both searches ',
      'in parallel, then merges results. ',
      'The `k=60` parameter balances ',
      'high vs. low ranked items. ',
      'This gives a robust fusion ',
      'that outperforms either signal alone.',
    ];

    let idx = 0;
    let cancelled = false;

    const timer = setInterval(() => {
      if (cancelled) return;
      if (idx < tokens.length) {
        onToken(tokens[idx]);
        idx++;
      } else {
        clearInterval(timer);
        onSources([
          {
            child_id: '01JBLK1042CHILD0000000000',
            parent_id: '01JBLK1042PARENT000000000',
            content: 'Architecture doc excerpt...',
            parent_content: '',
            score: 0.923,
            rerank_score: 0.91,
            namespace: 'default',
            content_hash: 'sha256:mock1042',
          },
          {
            child_id: '01JBLK1680CHILD0000000000',
            parent_id: '01JBLK1680PARENT000000000',
            content: 'Previous conversation...',
            parent_content: '',
            score: 0.781,
            rerank_score: 0.77,
            namespace: 'default',
            content_hash: 'sha256:mock1680',
          },
        ]);
        setTimeout(() => {
          if (!cancelled) onDone('conv-' + Date.now());
        }, 100);
      }
    }, 80);

    void onError; // suppress unused warning in mock
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  },

  // ─── Memory CRUD (MEM03) ──────────────────────────────────────────────────
  // Mutable in-memory store so create/edit/invalidate reflect in later lists.
  async listMemory(filter: MemoryListFilter): Promise<MemoryListResponse> {
    await delay(220);
    let items = memoryStore.filter((m) => m.user_id === (filter.user_id || m.user_id));
    if (!filter.include_invalidated) {
      items = items.filter((m) => m.status !== 'invalidated');
    }
    if (filter.memory_type) {
      items = items.filter((m) => m.memory_type === filter.memory_type);
    }
    // Strip B-class fields unless explain mode is on (phased rollout § 7.3).
    const projected = items.map((m) => (filter.explain ? m : stripExplain(m)));
    const page = filter.page ?? 0;
    const pageSize = filter.page_size ?? 20;
    const start = page * pageSize;
    const slice = projected.slice(start, start + pageSize);
    return {
      memories: slice,
      meta: {
        total: projected.length,
        page,
        page_size: pageSize,
        coverage_ratio: 1,
        latency_ms: 18,
        warnings: [],
      },
    };
  },

  async createMemory(req: MemoryCreateRequest): Promise<MemoryCreateResponse> {
    await delay(200);
    const now = Date.now();
    const id = `mem_${(nextMemId++).toString(16)}`;
    memoryStore.unshift({
      memory_id: id,
      content: req.content,
      memory_type: req.memory_type,
      status: 'active',
      user_id: req.user_id,
      extracted_at: now,
      invalidated_at: null,
      revoked_at: null,
      invalidated_by_block_id: null,
      extraction_method: 'user_created',
      source_session_id: null,
      source_interaction_id: null,
    });
    return { memory_id: id, extracted_at: now, extraction_method: 'user_created' };
  },

  async editMemory(id: string, req: MemoryEditRequest): Promise<MemoryEditResponse> {
    await delay(200);
    const idx = memoryStore.findIndex((m) => m.memory_id === id);
    if (idx === -1) throw new Error('404 not found');
    const old = memoryStore[idx];
    const now = Date.now();
    const newId = `mem_${(nextMemId++).toString(16)}`;
    // MEM03 § 5.2 — edit = insert new (user_edit) + invalidate old.
    memoryStore[idx] = {
      ...old,
      status: 'invalidated',
      invalidated_at: now,
      invalidated_by_block_id: newId,
    };
    memoryStore.unshift({
      memory_id: newId,
      content: req.new_content ?? old.content,
      memory_type: req.new_memory_type ?? old.memory_type,
      status: 'active',
      user_id: old.user_id,
      extracted_at: now,
      invalidated_at: null,
      revoked_at: null,
      invalidated_by_block_id: null,
      extraction_method: 'user_edit',
      source_session_id: old.source_session_id ?? null,
      source_interaction_id: old.source_interaction_id ?? null,
    });
    return { new_memory_id: newId, old_memory_id: id };
  },

  async invalidateMemory(id: string): Promise<MemoryInvalidateResponse> {
    await delay(200);
    const m = memoryStore.find((x) => x.memory_id === id);
    if (!m) throw new Error('404 not found');
    const now = Date.now();
    m.status = 'invalidated';
    m.invalidated_at = m.invalidated_at ?? now;
    return { block_id: id, status: 'invalidated' };
  },

  // ─── Namespace CRUD (F12) ─────────────────────────────────────────────────
  async getNamespaceDetail(name: string): Promise<NamespaceDetail> {
    await delay(180);
    const existing = namespaceDetailStore.find((n) => n.name === name);
    if (existing) return existing;
    // Synthesize a detail for namespaces only present in the simple list.
    return {
      ns_id: `ns_${name.slice(0, 4)}`,
      name,
      created_at: new Date().toISOString(),
      doc_count: 0,
      block_count: 0,
      description: '',
      isolation_mode: 'shared',
      visibility: 'tenant',
      status: 'active',
      owner_user_id: null,
      configs: emptyConfigs(),
    };
  },

  async createNamespaceFull(req: CreateNamespaceFullRequest): Promise<NamespaceDetail> {
    await delay(240);
    const detail: NamespaceDetail = {
      ns_id: `ns_${Math.random().toString(16).slice(2, 6)}`,
      name: req.name,
      created_at: new Date().toISOString(),
      doc_count: 0,
      block_count: 0,
      description: req.description ?? '',
      isolation_mode: req.isolation_mode ?? 'shared',
      visibility: req.visibility ?? 'tenant',
      status: 'active',
      owner_user_id: null,
      configs: { ...emptyConfigs(), ...(req.configs as Partial<NamespaceConfigs>) },
    };
    namespaceDetailStore.unshift(detail);
    return detail;
  },

  async updateNamespace(name: string, req: UpdateNamespaceRequest): Promise<NamespaceDetail> {
    await delay(220);
    const idx = namespaceDetailStore.findIndex((n) => n.name === name);
    const base = idx === -1 ? await this.getNamespaceDetail(name) : namespaceDetailStore[idx];
    const next: NamespaceDetail = {
      ...base,
      description: req.description ?? base.description,
      isolation_mode: req.isolation_mode ?? base.isolation_mode,
      visibility: req.visibility ?? base.visibility,
      configs: { ...base.configs, ...(req.configs as Partial<NamespaceConfigs>) },
    };
    if (idx === -1) namespaceDetailStore.unshift(next);
    else namespaceDetailStore[idx] = next;
    return next;
  },

  // ─── Auth (P08 — HttpOnly cookie model simulated in-memory) ────────────────
  // A real backend manages the session via cookies; standalone we keep a
  // module-level flag so the bootstrap → login → guarded-pages flow works.
  authMe(): AuthMeResponse | null {
    if (!mockSession.authed) return null;
    return {
      id: mockSession.user.id,
      email: mockSession.user.email,
      display_name: mockSession.user.display_name,
      role: mockSession.user.role,
      email_verified: true,
      edition: 'ce',
    };
  },

  async login(email: string, _password: string): Promise<AuthMeResponse> {
    await delay(280);
    void _password;
    // Demo heuristic: any email containing "admin" logs in as admin role.
    const role = /admin/i.test(email) ? 'admin' : 'user';
    mockSession.authed = true;
    mockSession.user = {
      id: role === 'admin' ? 'usr_admin' : 'usr_demo',
      email,
      display_name: email.split('@')[0],
      role,
    };
    return { ...mockSession.user, email_verified: true, edition: 'ce' };
  },

  logout(): void {
    mockSession.authed = false;
  },

  async bootstrap(token: string): Promise<BootstrapResponse> {
    await delay(260);
    if (!token || token.trim().length < 4) {
      throw new Error('Invalid bootstrap token');
    }
    return {
      admin_api_key: `cortrix_sk_mock_${Math.random().toString(36).slice(2, 18)}${Math.random()
        .toString(36)
        .slice(2, 18)}`,
      expires_at: null,
      scopes: ['admin:*'],
    };
  },

  // ─── Admin / Users (P08 § 2.13-bis) ───────────────────────────────────────
  async listUsers(filter: UserListFilter): Promise<UserListResponse> {
    await delay(200);
    let items = [...userStore];
    if (filter.q) {
      const q = filter.q.toLowerCase();
      items = items.filter(
        (u) => u.email.toLowerCase().includes(q) || (u.display_name ?? '').toLowerCase().includes(q),
      );
    }
    if (filter.status) items = items.filter((u) => u.status === filter.status);
    if (filter.role) items = items.filter((u) => u.role === filter.role);
    const page = filter.page ?? 1;
    const limit = filter.limit ?? 20;
    const start = (page - 1) * limit;
    return { users: items.slice(start, start + limit), total: items.length, page, limit };
  },

  async createUser(req: UserCreateRequest): Promise<UserMutationResponse> {
    await delay(220);
    if (userStore.some((u) => u.email.toLowerCase() === req.email.toLowerCase())) {
      throw new Error(JSON.stringify({ error: { code: 'CX_ERR_USER_EMAIL_EXISTS', message: 'Email already exists', retryable: false, category: 'permanent' } }));
    }
    const user: UserRecord = {
      id: `usr_${(nextUserId++).toString(16)}`,
      email: req.email,
      display_name: req.display_name ?? req.email.split('@')[0],
      role: req.role,
      status: 'active',
      email_verified: false,
      created_at: new Date().toISOString(),
    };
    userStore.unshift(user);
    return { user };
  },

  async updateUser(id: string, req: UserUpdateRequest): Promise<UserMutationResponse> {
    await delay(200);
    const idx = userStore.findIndex((u) => u.id === id);
    if (idx === -1) throw new Error(JSON.stringify({ error: { code: 'CX_ERR_USER_NOT_FOUND', message: 'User not found', retryable: false, category: 'permanent' } }));
    userStore[idx] = {
      ...userStore[idx],
      email: req.email ?? userStore[idx].email,
      display_name: req.display_name ?? userStore[idx].display_name,
      role: req.role ?? userStore[idx].role,
      email_verified: req.email_verified ?? userStore[idx].email_verified,
    };
    return { user: userStore[idx] };
  },

  async setUserStatus(id: string, status: UserStatus): Promise<UserMutationResponse> {
    await delay(180);
    const idx = userStore.findIndex((u) => u.id === id);
    if (idx === -1) throw new Error(JSON.stringify({ error: { code: 'CX_ERR_USER_NOT_FOUND', message: 'User not found', retryable: false, category: 'permanent' } }));
    userStore[idx] = { ...userStore[idx], status };
    return { user: userStore[idx] };
  },

  // ─── Operation Log (F18a CE — GET /operations) ────────────────────────────
  async listOperations(filter: OperationLogFilter): Promise<OperationLogResponse> {
    await delay(210);
    let items = [...operationStore];
    if (filter.user_id) items = items.filter((o) => o.user_id === filter.user_id);
    if (filter.action) items = items.filter((o) => o.action === filter.action);
    if (filter.resource_type) items = items.filter((o) => o.resource_type === filter.resource_type);
    if (filter.trace_id) items = items.filter((o) => o.trace_id === filter.trace_id);
    if (filter.from_timestamp != null) items = items.filter((o) => o.timestamp >= filter.from_timestamp!);
    if (filter.to_timestamp != null) items = items.filter((o) => o.timestamp <= filter.to_timestamp!);
    items.sort((a, b) => b.timestamp - a.timestamp);
    const offset = filter.offset ?? 0;
    const limit = filter.limit ?? 50;
    const slice = items.slice(offset, offset + limit);
    const total = items.length;
    const nextOffset = offset + limit;
    return {
      operations: slice,
      meta: { total_count: total, has_next: nextOffset < total, next_offset: nextOffset },
    };
  },

  // ─── API Keys (P08 § 2.13.3) ──────────────────────────────────────────────
  async listApiKeys(): Promise<ApiKeySummary[]> {
    await delay(160);
    return apiKeyStore.map((k) => ({ ...k }));
  },

  async createApiKey(req: ApiKeyCreateRequest): Promise<ApiKeyCreateResponse> {
    await delay(220);
    const id = `key_${(nextApiKeyId++).toString(16)}`;
    const rand = `${Math.random().toString(36).slice(2)}${Math.random().toString(36).slice(2)}`;
    const key = `cortrix_sk_${rand}`;
    const created_at = new Date().toISOString();
    apiKeyStore.unshift({
      id,
      name: req.name,
      key_prefix: key.slice(0, 16),
      created_at,
      last_used_at: null,
      expires_at: req.expires_at ?? null,
      status: 'active',
    });
    return { id, key, name: req.name, created_at, expires_at: req.expires_at ?? null };
  },

  revokeApiKey(id: string): void {
    const k = apiKeyStore.find((x) => x.id === id);
    if (k) k.status = 'revoked';
  },
};

// ─── Mock data stores (module-level, mutable) ─────────────────────────────────

let nextMemId = 1010;

function stripExplain(m: MemoryItem): MemoryItem {
  return {
    memory_id: m.memory_id,
    content: m.content,
    memory_type: m.memory_type,
    status: m.status,
    user_id: m.user_id,
    extracted_at: m.extracted_at,
    invalidated_at: m.invalidated_at ?? null,
    revoked_at: m.revoked_at ?? null,
  };
}

const memoryStore: MemoryItem[] = [
  {
    memory_id: 'mem_3f2',
    content: 'User switched their primary backend language from Python to Go.',
    memory_type: 'fact',
    status: 'active',
    user_id: 'user_demo',
    extracted_at: Date.parse('2026-05-18T09:30:00Z'),
    invalidated_at: null,
    revoked_at: null,
    invalidated_by_block_id: null,
    extraction_method: 'llm_extracted',
    source_session_id: 'sess_abc',
    source_interaction_id: 'int_991',
  },
  {
    memory_id: 'mem_4a1',
    content: 'Prefers concise answers with code examples first, prose second.',
    memory_type: 'preference',
    status: 'active',
    user_id: 'user_demo',
    extracted_at: Date.parse('2026-05-20T14:05:00Z'),
    invalidated_at: null,
    revoked_at: null,
    invalidated_by_block_id: null,
    extraction_method: 'user_created',
    source_session_id: null,
    source_interaction_id: null,
  },
  {
    memory_id: 'mem_2c9',
    content: 'Attended the Cortrix design review on 2026-05-12.',
    memory_type: 'event',
    status: 'active',
    user_id: 'user_demo',
    extracted_at: Date.parse('2026-05-12T17:00:00Z'),
    invalidated_at: null,
    revoked_at: Date.parse('2026-05-21T10:00:00Z'),
    invalidated_by_block_id: null,
    extraction_method: 'system',
    source_session_id: 'sess_xyz',
    source_interaction_id: 'int_120',
  },
  {
    memory_id: 'mem_1b0',
    content: 'Imported from legacy notes: timezone is Asia/Shanghai.',
    memory_type: 'fact',
    status: 'invalidated',
    user_id: 'user_demo',
    extracted_at: Date.parse('2026-04-30T08:00:00Z'),
    invalidated_at: Date.parse('2026-05-19T11:00:00Z'),
    revoked_at: null,
    invalidated_by_block_id: 'mem_4a1',
    extraction_method: 'imported',
    source_session_id: null,
    source_interaction_id: null,
  },
];

function emptyConfigs(): NamespaceConfigs {
  return {
    reranker_config: {},
    enricher_config: {},
    parser_config: {},
    memory_config: {},
    watcher_config: {},
    cleaning_config: {},
    rag_fusion_config: {},
    crag_config: {},
    complexity_config: {},
    sparse_config: {},
    doc_summary_config: {},
  };
}

const namespaceDetailStore: NamespaceDetail[] = [
  {
    ns_id: 'ns_8f2a',
    name: 'production-docs',
    created_at: '2026-04-12T00:00:00Z',
    doc_count: 842,
    block_count: 24108,
    description: 'Primary production knowledge base.',
    isolation_mode: 'shared',
    visibility: 'tenant',
    status: 'active',
    owner_user_id: 'user_demo',
    configs: {
      ...emptyConfigs(),
      reranker_config: { enabled: true, model: 'zerank-1-small', top_n: 20 },
      rag_fusion_config: { enabled: true, variant_count: 3 },
      sparse_config: { enabled: true, topk: 100, max_tokens: 8192 },
    },
    admission: {
      estimated_size_bytes: 104857600,
      budget_limit_bytes: 2147483648,
      quota_used: 3,
      quota_limit: 50,
    },
  },
  {
    ns_id: 'ns_3c71',
    name: 'legal-contracts',
    created_at: '2026-03-28T00:00:00Z',
    doc_count: 311,
    block_count: 9640,
    description: 'Contract corpus with stricter parsing.',
    isolation_mode: 'isolated',
    visibility: 'private',
    status: 'active',
    owner_user_id: 'user_demo',
    configs: {
      ...emptyConfigs(),
      parser_config: { strategy: 'layout-aware', ocr: true },
      doc_summary_config: { enabled: true, summary_length: 512, llm_model: 'claude-haiku' },
    },
    admission: {
      estimated_size_bytes: 41943040,
      budget_limit_bytes: 2147483648,
      quota_used: 3,
      quota_limit: 50,
    },
  },
];

// ─── Auth session (mutable, module-level) ─────────────────────────────────────
// Seeded as already-authenticated (admin) so standalone dev lands directly on
// the app shell without a live backend; LoginPage / logout still work.
const mockSession: { authed: boolean; user: { id: string; email: string; display_name?: string; role: 'admin' | 'user' } } = {
  authed: true,
  user: { id: 'usr_admin', email: 'admin@cortrix.local', display_name: 'admin', role: 'admin' },
};

// ─── Admin users (mutable) ────────────────────────────────────────────────────
let nextUserId = 0x200;

const userStore: UserRecord[] = [
  { id: 'usr_admin', email: 'admin@cortrix.local', display_name: 'Admin', role: 'admin', status: 'active', email_verified: true, created_at: '2026-04-01T08:00:00Z' },
  { id: 'usr_demo', email: 'alice@cortrix.local', display_name: 'Alice', role: 'user', status: 'active', email_verified: true, created_at: '2026-04-15T10:30:00Z' },
  { id: 'usr_bob', email: 'bob@cortrix.local', display_name: 'Bob', role: 'user', status: 'disabled', email_verified: false, created_at: '2026-05-02T14:05:00Z' },
];

// ─── Operation log (mutable) ──────────────────────────────────────────────────
const now = Date.now();
const operationStore: OperationLogEntry[] = [
  { id: 12348, timestamp: now - 60_000, user_id: 'usr_admin', action: 'user_created', namespace_id: null, resource_type: 'user', resource_id: 'usr_bob', summary: 'Created user bob@cortrix.local', status: 'success', trace_id: 'trace-9a1', session_id: 'sess-admin', source_ip: '127.0.0.1', user_agent: 'Mozilla/5.0', details_json: { role: 'user' }, failure_reason: null },
  { id: 12347, timestamp: now - 3_600_000, user_id: 'usr_demo', action: 'memory_create', namespace_id: 'production-docs', resource_type: 'memory', resource_id: 'block-789', summary: 'User likes concise answers', status: 'success', trace_id: 'trace-001', session_id: 'sess-abc', source_ip: '10.0.0.4', user_agent: 'cortrix-sdk/0.1', details_json: null, failure_reason: null },
  { id: 12346, timestamp: now - 7_200_000, user_id: 'usr_demo', action: 'ns_create', namespace_id: 'legal-contracts', resource_type: 'namespace', resource_id: 'legal-contracts', summary: 'Created namespace legal-contracts', status: 'success', trace_id: 'trace-77c', session_id: 'sess-abc', source_ip: '10.0.0.4', user_agent: 'cortrix-sdk/0.1', details_json: null, failure_reason: null },
  { id: 12345, timestamp: now - 86_400_000, user_id: 'usr_demo', action: 'query', namespace_id: 'production-docs', resource_type: 'query', resource_id: 'int_4501', summary: 'storage architecture trade-offs', status: 'success', trace_id: 'trace-120', session_id: 'sess-xyz', source_ip: '10.0.0.9', user_agent: 'Mozilla/5.0', details_json: null, failure_reason: null },
  { id: 12344, timestamp: now - 90_000_000, user_id: 'usr_demo', action: 'upload', namespace_id: 'production-docs', resource_type: 'document', resource_id: '842', summary: 'quarterly-report-2026-Q1.pdf', status: 'success', trace_id: null, session_id: null, source_ip: '10.0.0.9', user_agent: 'Mozilla/5.0', details_json: null, failure_reason: null },
];

// ─── API keys (mutable) ───────────────────────────────────────────────────────
let nextApiKeyId = 0x10;

const apiKeyStore: ApiKeySummary[] = [
  { id: 'key_1', name: 'production-agent', key_prefix: 'cortrix_sk_8f3a', created_at: '2026-05-10T09:00:00Z', last_used_at: '2026-06-01T12:00:00Z', expires_at: null, status: 'active' },
  { id: 'key_2', name: 'ci-pipeline', key_prefix: 'cortrix_sk_2b7c', created_at: '2026-04-22T14:30:00Z', last_used_at: null, expires_at: null, status: 'active' },
];
