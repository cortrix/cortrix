import { useState, useEffect, useCallback } from 'react';
import {
  CpuChipIcon, EyeIcon, ChatBubbleLeftRightIcon, CheckIcon, ArrowPathIcon, EyeSlashIcon,
  Cog6ToothIcon, ServerIcon, CircleStackIcon, FolderIcon, ShieldCheckIcon, KeyIcon,
  LockClosedIcon, PlusIcon, ClipboardIcon, TrashIcon, ExclamationTriangleIcon,
} from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useAppStore } from '../../store/useAppStore';
import type { LLMProvider } from '../../types/api';
import { listApiKeys, createApiKey, revokeApiKey } from '../../api/apiKeys';
import { parseAgentError } from '../../api/errors';
import { Button, Input, Modal, Badge, notify } from '../ui';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';

const AGENT_URL = '/agent';

// V1.0 agent config surface (cortrix-agent/routes/config.py). Only THREE endpoints
// exist: GET /config (current agent_llm), GET /config/providers (catalog, no models),
// PUT /config/agent_llm (forwards to the cortrix-server admin endpoint
// /api/v1/system/agent_llm_config). There is no per-role config and no provider-key
// endpoint — semantic_llm/vision_llm are config.yaml-only and are shown read-only here.

// Shape of GET /config (cortrix-agent AgentLlmConfigView; api_key is masked).
interface AgentLlmConfig {
  llm_provider: string;
  model: string;
  base_url?: string;
  api_key_masked?: string | null;
}

// ─── Shared helpers ────────────────────────────────────────────────────────────

function SaveButton({ saving, saved, onClick }: { saving: boolean; saved: boolean; onClick: () => void }) {
  const { t } = useTranslation();
  return (
    <button
      onClick={onClick}
      disabled={saving}
      className="flex items-center gap-1.5 px-4 py-2 rounded-lg bg-cortrix-600 hover:bg-cortrix-700 text-white text-sm font-medium disabled:opacity-50 transition-colors whitespace-nowrap"
    >
      {saving ? <ArrowPathIcon className="w-4 h-4 animate-spin" /> : saved ? <CheckIcon className="w-4 h-4" /> : null}
      {saved ? t('settings.saved') : t('settings.save')}
    </button>
  );
}

// ─── Provider color + letter ───────────────────────────────────────────────────

const PROVIDER_STYLE: Record<string, { letter: string; color: string }> = {
  glm:      { letter: 'G', color: 'bg-purple-100 text-purple-700 dark:bg-purple-900/30 dark:text-purple-400' },
  openai:   { letter: 'O', color: 'bg-ok/10 text-ok'  },
  claude:   { letter: 'C', color: 'bg-orange-100 text-orange-700 dark:bg-orange-900/30 dark:text-orange-400' },
  ollama:   { letter: 'L', color: 'bg-magma/15 text-magma-h'   },
  deepseek: { letter: 'D', color: 'bg-blue-100 text-blue-700 dark:bg-blue-900/30 dark:text-blue-400' },
  mock:     { letter: 'M', color: 'bg-surface2   text-muted'   },
};

function ProviderIcon({ id }: { id: string }) {
  const style = PROVIDER_STYLE[id] ?? { letter: '?', color: 'bg-surface2 text-muted' };
  return (
    <div className={`w-9 h-9 rounded-lg flex items-center justify-center text-sm font-bold shrink-0 ${style.color}`}>
      {style.letter}
    </div>
  );
}

// Read-only provider-type badge. The V1 agent exposes no per-provider `configured`
// flag (GET /config/providers is a catalog), so this reflects only the static
// provider kind, not a stored key state.
function ProviderTypeBadge({ provider }: { provider: LLMProvider }) {
  const { t } = useTranslation();
  if (!provider.needs_key && !provider.needs_url) {
    return (
      <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-magma/15 text-magma-h">
        <span className="w-1.5 h-1.5 rounded-full bg-magma" />
        {t('settings.providerBuiltIn')}
      </span>
    );
  }
  if (provider.needs_url) {
    return (
      <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-magma/15 text-magma-h">
        <span className="w-1.5 h-1.5 rounded-full bg-magma" />
        {t('settings.providerLocal')}
      </span>
    );
  }
  return (
    <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-surface2 text-muted">
      <span className="w-1.5 h-1.5 rounded-full bg-muted" />
      {t('settings.providerRequiresKey')}
    </span>
  );
}

// ─── Provider catalog card (read-only) ─────────────────────────────────────────
// The V1 agent has no PUT /config/providers/{id}; provider keys are set as part of
// the agent_llm config (below) or in config.yaml. This card is purely informational.

function ProviderCatalogCard({ provider, active }: { provider: LLMProvider; active: boolean }) {
  const { t } = useTranslation();
  return (
    <div className="bg-surface rounded-xl border border-line p-5">
      <div className="flex items-center gap-3">
        <ProviderIcon id={provider.id} />
        <div className="min-w-0">
          <p className="text-sm font-semibold text-txt truncate">{provider.name}</p>
          <ProviderTypeBadge provider={provider} />
        </div>
        {active && (
          <span className="ml-auto inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-ok/10 text-ok">
            <span className="w-1.5 h-1.5 rounded-full bg-ok" />
            {t('settings.providerActive')}
          </span>
        )}
      </div>
    </div>
  );
}

// ─── Agent LLM card (the only runtime-editable role) ───────────────────────────
// Saves via PUT /agent/config/agent_llm, which forwards to the cortrix-server admin
// endpoint PUT /api/v1/system/agent_llm_config (owns the authoritative C++
// IGlobalConfig, api_key encrypted at rest). The agent exposes no per-provider model
// catalog, so the model is a free-text field pre-filled from GET /config.

interface AgentLlmCardProps {
  providers: LLMProvider[];
  current: AgentLlmConfig;
  onSaved: (provider: string, model: string) => void;
}

function AgentLlmCard({ providers, current, onSaved }: AgentLlmCardProps) {
  const { t } = useTranslation();
  const [provider, setProvider] = useState(current.llm_provider);
  const [model,    setModel]    = useState(current.model);
  const [apiKey,   setApiKey]   = useState('');
  const [baseUrl,  setBaseUrl]  = useState(current.base_url ?? '');
  const [showKey,  setShowKey]  = useState(false);
  const [saving,   setSaving]   = useState(false);
  const [saved,    setSaved]    = useState(false);
  const [error,    setError]    = useState<string | null>(null);

  useEffect(() => {
    setProvider(current.llm_provider);
    setModel(current.model);
    setBaseUrl(current.base_url ?? '');
  }, [current.llm_provider, current.model, current.base_url]);

  const selected = providers.find((p) => p.id === provider);

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      const body: Record<string, string> = { provider, model };
      if (apiKey.trim())  body.api_key  = apiKey.trim();
      if (baseUrl.trim()) body.base_url = baseUrl.trim();

      const resp = await fetch(`${AGENT_URL}/config/agent_llm`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!resp.ok) throw new Error(await resp.text() || resp.statusText);

      setSaved(true);
      setApiKey('');  // clear after save
      setTimeout(() => setSaved(false), 2000);
      onSaved(provider, model);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="bg-surface rounded-xl border border-line p-5">
      <div className="flex items-start gap-6">
        {/* Left */}
        <div className="flex items-start gap-3 w-52 shrink-0">
          <div className="w-9 h-9 rounded-lg bg-cortrix-50 dark:bg-cortrix-800/20 flex items-center justify-center text-cortrix-600 dark:text-cortrix-400 shrink-0">
            <ChatBubbleLeftRightIcon className="w-5 h-5" />
          </div>
          <div className="min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <h3 className="text-sm font-semibold text-txt">{t('settings.agentLlmTitle')}</h3>
              <span className="text-[10px] font-mono px-1.5 py-0.5 rounded bg-surface2 text-muted">agent_llm</span>
            </div>
            <p className="text-xs text-muted mt-0.5 leading-relaxed">{t('settings.agentLlmDesc')}</p>
          </div>
        </div>

        {/* Right */}
        <div className="flex-1 min-w-0 space-y-3">
          <div className="flex items-end gap-3">
            <div className="flex-1 min-w-0">
              <label className="block text-xs font-medium text-muted mb-1">{t('settings.provider')}</label>
              <select
                value={provider}
                onChange={(e) => { setProvider(e.target.value); setSaved(false); }}
                className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
              >
                {providers.map((p) => (
                  <option key={p.id} value={p.id}>{p.name}</option>
                ))}
              </select>
            </div>
            <div className="flex-1 min-w-0">
              <label className="block text-xs font-medium text-muted mb-1">{t('settings.model')}</label>
              <input
                type="text"
                value={model}
                onChange={(e) => { setModel(e.target.value); setSaved(false); }}
                placeholder={t('settings.modelPlaceholder')}
                className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
              />
            </div>
            <SaveButton saving={saving} saved={saved} onClick={handleSave} />
          </div>

          {selected?.needs_key && (
            <div>
              <label className="block text-xs font-medium text-muted mb-1">
                {t('settings.apiKey')}
                <span className="text-xs text-muted ml-2">{t('settings.keyHint')}</span>
              </label>
              <div className="relative">
                <input
                  type={showKey ? 'text' : 'password'}
                  value={apiKey}
                  onChange={(e) => { setApiKey(e.target.value); setSaved(false); }}
                  placeholder={current.api_key_masked || t('settings.keyPlaceholder')}
                  className="w-full px-3 py-2 pr-9 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                />
                <button
                  type="button"
                  onClick={() => setShowKey((v) => !v)}
                  className="absolute right-2.5 top-1/2 -translate-y-1/2 text-muted hover:text-muted"
                >
                  {showKey ? <EyeSlashIcon className="w-4 h-4" /> : <EyeIcon className="w-4 h-4" />}
                </button>
              </div>
            </div>
          )}

          {selected?.needs_url && (
            <div>
              <label className="block text-xs font-medium text-muted mb-1">{t('settings.baseUrl')}</label>
              <input
                type="text"
                value={baseUrl}
                onChange={(e) => { setBaseUrl(e.target.value); setSaved(false); }}
                placeholder={t('settings.baseUrlPlaceholder')}
                className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
              />
            </div>
          )}

          {error && <p className="text-xs text-red-500 dark:text-red-400">{error}</p>}
        </div>
      </div>
    </div>
  );
}

// ─── Read-only role card (semantic_llm / vision_llm — config.yaml only) ─────────

function ReadOnlyRoleCard({
  icon, roleKey, title, description,
}: { icon: React.ReactNode; roleKey: string; title: string; description: string }) {
  const { t } = useTranslation();
  return (
    <div className="bg-surface rounded-xl border border-line p-5">
      <div className="flex items-start gap-6">
        <div className="flex items-start gap-3 w-52 shrink-0">
          <div className="w-9 h-9 rounded-lg bg-cortrix-50 dark:bg-cortrix-800/20 flex items-center justify-center text-cortrix-600 dark:text-cortrix-400 shrink-0">
            {icon}
          </div>
          <div className="min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <h3 className="text-sm font-semibold text-txt">{title}</h3>
              <span className="text-[10px] font-mono px-1.5 py-0.5 rounded bg-surface2 text-muted">{roleKey}</span>
            </div>
            <p className="text-xs text-muted mt-0.5 leading-relaxed">{description}</p>
          </div>
        </div>
        <div className="flex-1 min-w-0 flex items-center">
          <span className="inline-flex items-center gap-1.5 text-xs text-muted">
            <LockClosedIcon className="w-3.5 h-3.5" />
            {t('settings.configFileHint')}
          </span>
        </div>
      </div>
    </div>
  );
}

// ─── Info Row ─────────────────────────────────────────────────────────────────

function InfoRow({ label, value, mono = false }: { label: string; value: React.ReactNode; mono?: boolean }) {
  return (
    <div className="flex items-center justify-between py-2.5 border-b border-line last:border-0">
      <span className="text-sm text-muted">{label}</span>
      <span className={`text-sm font-medium text-txt ${mono ? 'font-mono' : ''}`}>
        {value}
      </span>
    </div>
  );
}

function StatusBadge({ ok, label }: { ok: boolean; label: string }) {
  return (
    <span className={`inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium ${
      ok
        ? 'bg-ok/10 text-ok'
        : 'bg-surface2 text-muted'
    }`}>
      <span className={`w-1.5 h-1.5 rounded-full ${ok ? 'bg-ok' : 'bg-muted'}`} />
      {label}
    </span>
  );
}

function Section({ icon, title, children }: { icon: React.ReactNode; title: string; children: React.ReactNode }) {
  return (
    <section className="mb-8">
      <div className="flex items-center gap-2 mb-4">
        <span className="text-muted">{icon}</span>
        <h2 className="text-sm font-semibold uppercase tracking-wider text-muted">{title}</h2>
      </div>
      {children}
    </section>
  );
}

// ─── Main Settings Page ───────────────────────────────────────────────────────

const EMPTY_AGENT_LLM: AgentLlmConfig = { llm_provider: '', model: '', base_url: '', api_key_masked: null };

export function SettingsPage() {
  const { t } = useTranslation();
  const { systemStatus } = useAppStore();

  const [loading,   setLoading]   = useState(true);
  const [providers, setProviders] = useState<LLMProvider[]>([]);
  const [agentLlm,  setAgentLlm]  = useState<AgentLlmConfig>(EMPTY_AGENT_LLM);

  // Load the V1 agent config surface: the provider catalog (GET /config/providers)
  // and the current agent_llm (GET /config). Both degrade to empty on failure so the
  // page renders without throwing (the removed /config/llm/roles call used to 404).
  const loadConfig = useCallback(() => {
    setLoading(true);
    Promise.all([
      fetch(`${AGENT_URL}/config/providers`)
        .then((r) => (r.ok ? r.json() : { providers: [] }))
        .catch(() => ({ providers: [] })),
      fetch(`${AGENT_URL}/config`)
        .then((r) => (r.ok ? r.json() : null))
        .catch(() => null),
    ])
      .then(([cat, cfg]: [{ providers?: LLMProvider[] }, AgentLlmConfig | null]) => {
        setProviders(cat.providers ?? []);
        if (cfg) setAgentLlm({ ...EMPTY_AGENT_LLM, ...cfg });
      })
      .finally(() => setLoading(false));
  }, []);

  useEffect(() => { loadConfig(); }, [loadConfig]);

  const handleAgentLlmSaved = (provider: string, model: string) => {
    setAgentLlm((prev) => ({ ...prev, llm_provider: provider, model }));
  };

  const semanticEnabled = !!systemStatus?.llm_enabled;

  return (
    <div className="max-w-4xl mx-auto px-6 py-8">
      {/* Page header */}
      <div className="flex items-center gap-3 mb-10">
        <div className="w-10 h-10 rounded-xl bg-cortrix-50 dark:bg-cortrix-800/20 flex items-center justify-center text-cortrix-600 dark:text-cortrix-400">
          <Cog6ToothIcon className="w-5 h-5" />
        </div>
        <div>
          <h1 className="text-xl font-bold text-txt">{t('settings.pageTitle')}</h1>
          <p className="text-sm text-muted">{t('settings.pageSubtitle')}</p>
        </div>
      </div>

      {/* ── Section 1: Provider catalog (read-only) ── */}
      <Section icon={<KeyIcon className="w-4 h-4" />} title={t('settings.providerSection')}>
        <p className="text-xs text-muted mb-4">{t('settings.providerCatalogDesc')}</p>
        {loading ? (
          <div className="flex items-center gap-2 text-muted py-4">
            <ArrowPathIcon className="w-4 h-4 animate-spin" />
            <span className="text-sm">{t('common.loading')}</span>
          </div>
        ) : (
          <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
            {providers.map((p) => (
              <ProviderCatalogCard key={p.id} provider={p} active={p.id === agentLlm.llm_provider} />
            ))}
          </div>
        )}
      </Section>

      {/* ── Section 2: LLM Role Configuration ── */}
      <Section icon={<CpuChipIcon className="w-4 h-4" />} title={t('settings.llmSection')}>
        {loading ? (
          <div className="flex items-center gap-2 text-muted py-4">
            <ArrowPathIcon className="w-4 h-4 animate-spin" />
            <span className="text-sm">{t('common.loading')}</span>
          </div>
        ) : (
          <div className="flex flex-col gap-3">
            <AgentLlmCard providers={providers} current={agentLlm} onSaved={handleAgentLlmSaved} />
            <ReadOnlyRoleCard
              icon={<CpuChipIcon className="w-5 h-5" />}
              roleKey="semantic_llm"
              title={t('settings.semanticLlmTitle')}
              description={t('settings.semanticLlmDesc')}
            />
            <ReadOnlyRoleCard
              icon={<EyeIcon className="w-5 h-5" />}
              roleKey="vision_llm"
              title={t('settings.visionLlmTitle')}
              description={t('settings.visionLlmDesc')}
            />
          </div>
        )}
        <p className="mt-3 text-xs text-muted">
          {t('settings.configFileNote')}
        </p>
      </Section>

      {/* ── Section 3: System Status ── */}
      <Section icon={<ServerIcon className="w-4 h-4" />} title={t('settings.systemSection')}>
        <div className="bg-surface rounded-xl border border-line px-5 divide-y divide-line">
          <InfoRow label={t('settings.systemVersion')} value={systemStatus?.version ?? '—'} mono />
          <InfoRow label={t('settings.systemUptime')} value={
            systemStatus?.uptime_seconds != null ? formatUptime(systemStatus.uptime_seconds) : '—'
          } />
          <InfoRow label={t('settings.systemLlm')} value={
            <StatusBadge ok={semanticEnabled} label={
              semanticEnabled ? (systemStatus?.llm_provider || t('settings.llmOn')) : t('settings.llmOff')
            } />
          } />
          <InfoRow label={t('settings.systemDocs')}       value={systemStatus?.total_doc_count?.toLocaleString()  ?? '—'} />
          <InfoRow label={t('settings.systemBlocks')}     value={systemStatus?.total_block_count?.toLocaleString() ?? '—'} />
          <InfoRow label={t('settings.systemNamespaces')} value={systemStatus?.namespace_count?.toLocaleString()   ?? '—'} />
        </div>
      </Section>

      {/* ── Section 4: Embedding Model ── */}
      <Section icon={<CircleStackIcon className="w-4 h-4" />} title={t('settings.embeddingSection')}>
        <div className="bg-surface rounded-xl border border-line px-5 divide-y divide-line">
          <InfoRow label={t('settings.embeddingModel')} value="bge-m3" mono />
          <InfoRow label={t('settings.embeddingDim')}   value="1024"   mono />
          <InfoRow label={t('settings.embeddingAccel')} value="CoreML (macOS)" />
          <InfoRow label={t('settings.embeddingNote')}  value={
            <span className="text-xs text-muted">{t('settings.embeddingConfigHint')}</span>
          } />
        </div>
      </Section>

      {/* ── Section 5: Watch Directory ── */}
      <Section icon={<FolderIcon className="w-4 h-4" />} title={t('settings.watchSection')}>
        <div className="bg-surface rounded-xl border border-line px-5 divide-y divide-line">
          <InfoRow label={t('settings.watchStatus')} value={
            <StatusBadge ok={false} label={t('settings.watchConfigHint')} />
          } />
          <InfoRow label={t('settings.watchNote')} value={
            <span className="text-xs text-muted">{t('settings.watchConfigFile')}</span>
          } />
        </div>
      </Section>

      {/* ── Section 6: Authentication ── */}
      <Section icon={<ShieldCheckIcon className="w-4 h-4" />} title={t('settings.authSection')}>
        <div className="bg-surface rounded-xl border border-line px-5 divide-y divide-line">
          <InfoRow label={t('settings.authStatus')} value={
            <StatusBadge ok={false} label={t('settings.authDisabled')} />
          } />
          <InfoRow label={t('settings.authNote')} value={
            <span className="text-xs text-muted">{t('settings.authConfigHint')}</span>
          } />
        </div>
      </Section>

      {/* ── Section 7: API Keys (P02a § 9.3 — user-level API keys, P08 § 2.13.3) ── */}
      <ApiKeysSection />
    </div>
  );
}

// ─── API Keys section (P02a § 9.3 — user-level API keys, P08 § 2.13.3) ─────────
// R1 left a disabled scaffold; R3/S5 fills it in: list / create (plaintext key
// shown once) / revoke, via TanStack Query. Standalone-backed by the mock.

function ApiKeysSection() {
  const { t } = useTranslation();
  const qc = useQueryClient();
  const [createOpen, setCreateOpen] = useState(false);
  const [newName, setNewName] = useState('');
  const [createdKey, setCreatedKey] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const { data: keys = [], isLoading } = useQuery({ queryKey: ['api-keys'], queryFn: listApiKeys });
  const invalidate = () => void qc.invalidateQueries({ queryKey: ['api-keys'] });

  const createMut = useMutation({
    mutationFn: (name: string) => createApiKey({ name }),
    onSuccess: (res) => {
      setCreatedKey(res.key);
      setNewName('');
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const revokeMut = useMutation({
    mutationFn: (id: string) => revokeApiKey(id),
    onSuccess: () => {
      notify.success(t('settings.apiKeyRevoked'));
      invalidate();
    },
    onError: (e) => notify.error(parseAgentError(e).message),
  });

  const closeModal = () => {
    setCreateOpen(false);
    setCreatedKey(null);
    setNewName('');
    setCopied(false);
  };

  const copyKey = async () => {
    if (!createdKey) return;
    try {
      await navigator.clipboard.writeText(createdKey);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch {
      notify.error(t('auth.bootstrap.copyFailed'));
    }
  };

  return (
    <Section icon={<KeyIcon className="w-4 h-4" />} title={t('settings.apiKeysSection')}>
      <div className="bg-surface rounded-xl border border-line p-5 space-y-4" data-testid="api-keys-section">
        <div className="flex items-start justify-between gap-4">
          <p className="text-xs text-muted leading-relaxed max-w-md">{t('settings.apiKeysSectionDesc')}</p>
          <Button
            size="sm"
            leftIcon={<PlusIcon className="w-4 h-4" />}
            onClick={() => setCreateOpen(true)}
            data-testid="api-key-create-btn"
          >
            {t('settings.apiKeysCreate')}
          </Button>
        </div>

        <ProgrammaticBanner
          comment={t('settings.apiKeysSdkComment')}
          snippet={
            <>
              <span className="text-magma-h">client</span>.<span className="text-amber">api_keys</span>.
              <span className="text-amber">create</span>(<span className="text-txt">name</span>=
              <span className="text-ok">&quot;my-agent-key&quot;</span>)
            </>
          }
        />

        {isLoading ? (
          <div className="py-8 text-center text-sm text-muted">{t('common.loading')}</div>
        ) : keys.length === 0 ? (
          <div className="rounded-md border border-dashed border-line py-8 text-center">
            <KeyIcon className="w-8 h-8 mx-auto mb-2 text-muted" />
            <p className="text-sm text-muted">{t('settings.apiKeysEmpty')}</p>
          </div>
        ) : (
          <table className="w-full text-sm" data-testid="api-keys-table">
            <thead>
              <tr className="border-b border-line text-left text-[11px] font-semibold uppercase tracking-wider text-muted">
                <th className="py-2 pr-3">{t('settings.apiKeyName')}</th>
                <th className="py-2 pr-3">{t('settings.apiKeyPrefix')}</th>
                <th className="py-2 pr-3">{t('settings.apiKeyStatus')}</th>
                <th className="py-2 pr-3">{t('settings.apiKeyLastUsed')}</th>
                <th className="py-2 text-right">{t('namespace.actions')}</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-line">
              {keys.map((k) => (
                <tr key={k.id} data-api-key-id={k.id}>
                  <td className="py-2.5 pr-3 font-medium text-txt">{k.name}</td>
                  <td className="py-2.5 pr-3 font-mono text-xs text-muted">{k.key_prefix}…</td>
                  <td className="py-2.5 pr-3">
                    <Badge variant={k.status === 'active' ? 'ok' : 'warning'}>{k.status}</Badge>
                  </td>
                  <td className="py-2.5 pr-3 font-mono text-xs text-muted">
                    {k.last_used_at ? k.last_used_at.slice(0, 10) : '—'}
                  </td>
                  <td className="py-2.5 text-right">
                    {k.status === 'active' && (
                      <Button
                        size="sm"
                        variant="danger"
                        leftIcon={<TrashIcon className="w-4 h-4" />}
                        loading={revokeMut.isPending && revokeMut.variables === k.id}
                        onClick={() => revokeMut.mutate(k.id)}
                        data-testid="api-key-revoke-btn"
                      >
                        {t('settings.apiKeyRevoke')}
                      </Button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      <Modal
        open={createOpen}
        onClose={closeModal}
        size="sm"
        title={t('settings.apiKeysCreate')}
        footer={
          createdKey ? (
            <Button onClick={closeModal}>{t('common.close')}</Button>
          ) : (
            <>
              <Button variant="secondary" onClick={closeModal}>
                {t('common.cancel')}
              </Button>
              <Button
                onClick={() => createMut.mutate(newName.trim())}
                loading={createMut.isPending}
                disabled={!newName.trim()}
                data-testid="api-key-create-submit"
              >
                {t('common.create')}
              </Button>
            </>
          )
        }
      >
        {createdKey ? (
          <div className="space-y-3">
            <div className="flex items-center gap-2 rounded-md border border-amber/40 bg-amber/10 px-3 py-2 text-xs text-amber" role="alert">
              <ExclamationTriangleIcon className="h-4 w-4 shrink-0" />
              {t('settings.apiKeyShownOnce')}
            </div>
            <div className="flex items-stretch gap-2">
              <code className="flex-1 overflow-x-auto rounded-md border border-line bg-codebg px-3 py-2.5 font-mono text-[13px] text-txt" data-testid="api-key-plaintext">
                {createdKey}
              </code>
              <Button
                variant="secondary"
                onClick={() => void copyKey()}
                leftIcon={copied ? <CheckIcon className="h-4 w-4" /> : <ClipboardIcon className="h-4 w-4" />}
              >
                {copied ? t('auth.bootstrap.copied') : t('common.copy')}
              </Button>
            </div>
          </div>
        ) : (
          <Input
            label={t('settings.apiKeyName')}
            value={newName}
            onChange={(e) => setNewName(e.target.value)}
            placeholder="my-agent-key"
            onKeyDown={(e) => e.key === 'Enter' && newName.trim() && createMut.mutate(newName.trim())}
            data-testid="api-key-name-input"
          />
        )}
      </Modal>
    </Section>
  );
}

function formatUptime(seconds: number): string {
  if (seconds < 60)   return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
}
