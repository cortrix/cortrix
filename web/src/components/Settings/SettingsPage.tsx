import { useState, useEffect, useCallback } from 'react';
import {
  CpuChipIcon, EyeIcon, ChatBubbleLeftRightIcon, CheckIcon, ArrowPathIcon, EyeSlashIcon,
  Cog6ToothIcon, ServerIcon, CircleStackIcon, FolderIcon, ShieldCheckIcon, KeyIcon,
  PlusIcon, ClipboardIcon, TrashIcon, ExclamationTriangleIcon,
} from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useAppStore } from '../../store/useAppStore';
import type { LLMRolesResponse, LLMProvider, LLMModel } from '../../types/api';
import { listApiKeys, createApiKey, revokeApiKey } from '../../api/apiKeys';
import { parseAgentError } from '../../api/errors';
import { Button, Input, Modal, Badge, notify } from '../ui';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';

const AGENT_URL = '/agent';

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

function ProviderBadge({ provider }: { provider: LLMProvider }) {
  const { t } = useTranslation();
  if (!provider.needs_key && !provider.needs_url) {
    return (
      <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-magma/15 text-magma-h">
        <span className="w-1.5 h-1.5 rounded-full bg-magma" />
        {t('settings.providerBuiltIn')}
      </span>
    );
  }
  if (!provider.needs_key && provider.needs_url) {
    return (
      <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-magma/15 text-magma-h">
        <span className="w-1.5 h-1.5 rounded-full bg-magma" />
        {t('settings.providerLocal')}
      </span>
    );
  }
  return provider.configured ? (
    <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-ok/10 text-ok">
      <span className="w-1.5 h-1.5 rounded-full bg-ok" />
      {t('settings.providerConfigured')}
    </span>
  ) : (
    <span className="inline-flex items-center gap-1 px-2 py-0.5 rounded-full text-xs font-medium bg-surface2 text-muted">
      <span className="w-1.5 h-1.5 rounded-full bg-muted" />
      {t('settings.providerNotConfigured')}
    </span>
  );
}

// ─── Provider Config Card ──────────────────────────────────────────────────────

interface ProviderCardProps {
  provider: LLMProvider;
  onSaved: (id: string) => void;
}

function ProviderConfigCard({ provider, onSaved }: ProviderCardProps) {
  const { t } = useTranslation();
  const [apiKey,  setApiKey]  = useState('');
  const [baseUrl, setBaseUrl] = useState(provider.base_url || '');
  const [showKey, setShowKey] = useState(false);
  const [saving,  setSaving]  = useState(false);
  const [saved,   setSaved]   = useState(false);
  const [error,   setError]   = useState<string | null>(null);

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      const body: Record<string, string> = {};
      if (provider.needs_key && apiKey.trim()) body.api_key = apiKey.trim();
      if (provider.needs_url && baseUrl.trim()) body.base_url = baseUrl.trim();

      const resp = await fetch(`${AGENT_URL}/config/providers/${provider.id}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!resp.ok) throw new Error(await resp.text() || resp.statusText);

      setSaved(true);
      if (provider.needs_key) setApiKey('');  // clear after save
      setTimeout(() => setSaved(false), 2000);
      onSaved(provider.id);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  };

  const noConfig = !provider.needs_key && !provider.needs_url;

  return (
    <div className="bg-surface rounded-xl border border-line p-5">
      <div className="flex items-center gap-6">
        {/* Left: identity */}
        <div className="flex items-center gap-3 w-52 shrink-0">
          <ProviderIcon id={provider.id} />
          <div className="min-w-0">
            <p className="text-sm font-semibold text-txt truncate">{provider.name}</p>
            <ProviderBadge provider={provider} />
          </div>
        </div>

        {/* Right: input or status */}
        {noConfig ? (
          <p className="flex-1 text-sm text-muted">{t('settings.noConfigNeeded')}</p>
        ) : (
          <div className="flex-1 min-w-0">
            <div className="flex items-end gap-3">
              {provider.needs_key && (
                <div className="flex-1 min-w-0">
                  <label className="block text-xs font-medium text-muted mb-1">
                    {t('settings.apiKey')}
                  </label>
                  <div className="relative">
                    <input
                      type={showKey ? 'text' : 'password'}
                      value={apiKey}
                      onChange={(e) => setApiKey(e.target.value)}
                      placeholder={provider.configured ? t('settings.keyHint') : t('settings.keyPlaceholder')}
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
              {provider.needs_url && (
                <div className="flex-1 min-w-0">
                  <label className="block text-xs font-medium text-muted mb-1">
                    {t('settings.baseUrl')}
                  </label>
                  <input
                    type="text"
                    value={baseUrl}
                    onChange={(e) => setBaseUrl(e.target.value)}
                    placeholder={t('settings.baseUrlPlaceholder')}
                    className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                  />
                </div>
              )}
              <SaveButton saving={saving} saved={saved} onClick={handleSave} />
            </div>
            {error && <p className="text-xs text-red-500 dark:text-red-400 mt-2">{error}</p>}
          </div>
        )}
      </div>
    </div>
  );
}

// ─── LLM Role Card (horizontal) ───────────────────────────────────────────────

interface RoleCardProps {
  icon: React.ReactNode;
  roleKey: string;
  title: string;
  description: string;
  provider: string;
  model: string;
  providers: LLMProvider[];
  models: Record<string, LLMModel[]>;
  visionOnly?: boolean;
  onSave: (provider: string, model: string) => Promise<void>;
}

function LLMRoleCard({
  icon, roleKey, title, description,
  provider, model,
  providers, models,
  visionOnly = false,
  onSave,
}: RoleCardProps) {
  const { t } = useTranslation();
  const [selectedProvider, setSelectedProvider] = useState(provider);
  const [selectedModel,    setSelectedModel]    = useState(model);
  const [saving, setSaving] = useState(false);
  const [saved,  setSaved]  = useState(false);
  const [error,  setError]  = useState<string | null>(null);

  useEffect(() => {
    setSelectedProvider(provider);
    setSelectedModel(model);
  }, [provider, model]);

  const currentModels = (models[selectedProvider] || []).filter(
    (m) => !visionOnly || m.caps?.includes('vision'),
  );

  const handleProviderChange = (pid: string) => {
    setSelectedProvider(pid);
    const ms = (models[pid] || []).filter((m) => !visionOnly || m.caps?.includes('vision'));
    if (ms.length > 0) setSelectedModel(ms[0].id);
    setSaved(false);
    setError(null);
  };

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      await onSave(selectedProvider, selectedModel);
      setSaved(true);
      setTimeout(() => setSaved(false), 2000);
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
            {icon}
          </div>
          <div className="min-w-0">
            <div className="flex items-center gap-2 flex-wrap">
              <h3 className="text-sm font-semibold text-txt">{title}</h3>
              <span className="text-[10px] font-mono px-1.5 py-0.5 rounded bg-surface2 text-muted">
                {roleKey}
              </span>
            </div>
            <p className="text-xs text-muted mt-0.5 leading-relaxed">{description}</p>
          </div>
        </div>

        {/* Right */}
        <div className="flex-1 min-w-0">
          {providers.length === 0 ? (
            <p className="text-sm text-muted italic">{t('settings.noProviderAvailable')}</p>
          ) : (
            <div className="flex items-end gap-3">
              <div className="flex-1 min-w-0">
                <label className="block text-xs font-medium text-muted mb-1">
                  {t('settings.provider')}
                </label>
                <select
                  value={selectedProvider}
                  onChange={(e) => handleProviderChange(e.target.value)}
                  className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                >
                  {providers.map((p) => (
                    <option key={p.id} value={p.id}>{p.name}</option>
                  ))}
                </select>
              </div>
              <div className="flex-1 min-w-0">
                <label className="block text-xs font-medium text-muted mb-1">
                  {t('settings.model')}
                </label>
                <select
                  value={selectedModel}
                  onChange={(e) => { setSelectedModel(e.target.value); setSaved(false); }}
                  className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                >
                  {currentModels.map((m) => (
                    <option key={m.id} value={m.id}>{m.name} — {m.description}</option>
                  ))}
                </select>
              </div>
              <SaveButton saving={saving} saved={saved} onClick={handleSave} />
            </div>
          )}
          {error && <p className="text-xs text-red-500 dark:text-red-400 mt-2">{error}</p>}
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

type RoleKey = 'semantic_llm' | 'vision_llm' | 'agent_llm';

const EMPTY_ROLES = {
  semantic_llm: { provider: '', model: '' },
  vision_llm:   { provider: '', model: '' },
  agent_llm:    { provider: '', model: '' },
};

export function SettingsPage() {
  const { t } = useTranslation();
  const { systemStatus } = useAppStore();

  const [loading,   setLoading]   = useState(true);
  const [providers, setProviders] = useState<LLMProvider[]>([]);
  const [models,    setModels]    = useState<Record<string, LLMModel[]>>({});
  const [roles,     setRoles]     = useState(EMPTY_ROLES);

  const loadRoles = useCallback(() => {
    setLoading(true);
    fetch(`${AGENT_URL}/config/llm/roles`)
      .then((r) => r.json())
      .then((data: LLMRolesResponse) => {
        setProviders(data.providers ?? []);
        setModels(data.models ?? {});
        setRoles({ ...EMPTY_ROLES, ...(data.roles ?? {}) });
      })
      .catch(() => {})
      .finally(() => setLoading(false));
  }, []);

  useEffect(() => { loadRoles(); }, [loadRoles]);

  // After a provider key is saved, refresh the full providers list so role
  // dropdowns immediately show the newly-configured provider.
  const handleProviderSaved = (_id: string) => {
    fetch(`${AGENT_URL}/config/llm/roles`)
      .then((r) => r.json())
      .then((data: LLMRolesResponse) => {
        setProviders(data.providers ?? []);
        setModels(data.models ?? {});
      })
      .catch(() => {});
  };

  const handleSaveRole = (role: RoleKey) => async (provider: string, model: string) => {
    const resp = await fetch(`${AGENT_URL}/config/llm/roles/${role}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ provider, model }),
    });
    if (!resp.ok) throw new Error(await resp.text() || resp.statusText);
    setRoles((prev) => ({ ...prev, [role]: { provider, model } }));
  };

  // Only configured providers appear in role dropdowns
  const configuredProviders = providers.filter((p) => p.configured);
  const semanticEnabled     = !!systemStatus?.llm_enabled;

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

      {/* ── Section 1: Provider API Keys ── */}
      <Section icon={<KeyIcon className="w-4 h-4" />} title={t('settings.providerSection')}>
        <p className="text-xs text-muted mb-4">{t('settings.providerSectionDesc')}</p>
        {loading ? (
          <div className="flex items-center gap-2 text-muted py-4">
            <ArrowPathIcon className="w-4 h-4 animate-spin" />
            <span className="text-sm">{t('common.loading')}</span>
          </div>
        ) : (
          <div className="flex flex-col gap-3">
            {providers.map((p) => (
              <ProviderConfigCard key={p.id} provider={p} onSaved={handleProviderSaved} />
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
            <LLMRoleCard
              icon={<CpuChipIcon className="w-5 h-5" />}
              roleKey="semantic_llm"
              title={t('settings.semanticLlmTitle')}
              description={t('settings.semanticLlmDesc')}
              provider={roles.semantic_llm.provider}
              model={roles.semantic_llm.model}
              providers={configuredProviders}
              models={models}
              onSave={handleSaveRole('semantic_llm')}
            />
            <LLMRoleCard
              icon={<EyeIcon className="w-5 h-5" />}
              roleKey="vision_llm"
              title={t('settings.visionLlmTitle')}
              description={t('settings.visionLlmDesc')}
              provider={roles.vision_llm.provider}
              model={roles.vision_llm.model}
              providers={configuredProviders}
              models={models}
              visionOnly={true}
              onSave={handleSaveRole('vision_llm')}
            />
            <LLMRoleCard
              icon={<ChatBubbleLeftRightIcon className="w-5 h-5" />}
              roleKey="agent_llm"
              title={t('settings.agentLlmTitle')}
              description={t('settings.agentLlmDesc')}
              provider={roles.agent_llm.provider}
              model={roles.agent_llm.model}
              providers={configuredProviders}
              models={models}
              onSave={handleSaveRole('agent_llm')}
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

      {/* ── Section 7: API Keys (P08 Bootstrap — framework only, R1-S1) ──
          Scaffold for user-level API key management. The full flow (Bootstrap
          URL -> useAuthStore -> LoginPage -> list/create/revoke) lands in
          P02a-R3/S5 (P02a design § 9.3). R1 only stands up the section shell. */}
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
