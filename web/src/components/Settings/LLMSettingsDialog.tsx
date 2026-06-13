import { useState, useEffect } from 'react';
import { XMarkIcon, CheckIcon, ArrowPathIcon, EyeIcon, EyeSlashIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import i18n from 'i18next';
import type { LLMProvidersResponse, LLMProvider, LLMModel } from '../../types/api';

const AGENT_URL = '/agent';

interface Props {
  open: boolean;
  onClose: () => void;
  onSaved?: (provider: string, model: string) => void;
}

export function LLMSettingsDialog({ open, onClose, onSaved }: Props) {
  const { t } = useTranslation();
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState(false);

  const [providers, setProviders] = useState<LLMProvider[]>([]);
  const [models, setModels] = useState<Record<string, LLMModel[]>>({});
  const [selectedProvider, setSelectedProvider] = useState('');
  const [selectedModel, setSelectedModel] = useState('');
  const [apiKey, setApiKey] = useState('');
  const [baseUrl, setBaseUrl] = useState('');
  const [showKey, setShowKey] = useState(false);

  useEffect(() => {
    if (!open) return;
    setLoading(true);
    setError(null);
    setSuccess(false);

    const lang = i18n.language || 'en';
    fetch(`${AGENT_URL}/config/llm/providers?lang=${lang}`)
      .then((r) => r.json())
      .then((data: LLMProvidersResponse) => {
        setProviders(data.providers);
        setModels(data.models);
        setSelectedProvider(data.current.provider);
        setSelectedModel(data.current.model);
        setApiKey('');
        setBaseUrl('');
      })
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false));
  }, [open]);

  const currentProvider = providers.find((p) => p.id === selectedProvider);
  const currentModels = models[selectedProvider] || [];

  const handleProviderChange = (pid: string) => {
    setSelectedProvider(pid);
    const ms = models[pid] || [];
    if (ms.length > 0) setSelectedModel(ms[0].id);
    setApiKey('');
    setBaseUrl('');
    setSuccess(false);
  };

  const handleSave = async () => {
    setSaving(true);
    setError(null);
    setSuccess(false);
    try {
      const body: Record<string, string> = {
        provider: selectedProvider,
        model: selectedModel,
      };
      if (apiKey) body.api_key = apiKey;
      if (baseUrl) body.base_url = baseUrl;

      // D3.5 r2 · Wave P · P4: the agent's PUT /config/agent_llm forwards to the
      // cortrix-server admin endpoint PUT /api/v1/system/agent_llm_config, which
      // owns the authoritative C++ IGlobalConfig (api_key encrypted at rest).
      const resp = await fetch(`${AGENT_URL}/config/agent_llm`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!resp.ok) {
        const text = await resp.text();
        throw new Error(text || resp.statusText);
      }
      setSuccess(true);
      onSaved?.(selectedProvider, selectedModel);
      setTimeout(() => onClose(), 800);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  };

  if (!open) return null;

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/40">
      <div className="bg-surface rounded-xl shadow-2xl w-full max-w-md mx-4 overflow-hidden">
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-line">
          <h2 className="text-base font-semibold text-txt">
            {t('settings.llmTitle')}
          </h2>
          <button
            onClick={onClose}
            className="p-1 rounded-md hover:bg-surface2 text-muted"
          >
            <XMarkIcon className="w-5 h-5" />
          </button>
        </div>

        {/* Body */}
        <div className="px-5 py-4 space-y-4">
          {loading ? (
            <div className="flex items-center justify-center py-8 text-muted">
              <ArrowPathIcon className="w-5 h-5 animate-spin mr-2" />
              {t('common.loading')}
            </div>
          ) : (
            <>
              {/* Provider */}
              <div>
                <label className="block text-sm font-medium text-txt mb-1">
                  {t('settings.provider')}
                </label>
                <select
                  value={selectedProvider}
                  onChange={(e) => handleProviderChange(e.target.value)}
                  className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                >
                  {providers.map((p) => (
                    <option key={p.id} value={p.id}>
                      {p.name}
                    </option>
                  ))}
                </select>
              </div>

              {/* Model */}
              <div>
                <label className="block text-sm font-medium text-txt mb-1">
                  {t('settings.model')}
                </label>
                <select
                  value={selectedModel}
                  onChange={(e) => { setSelectedModel(e.target.value); setSuccess(false); }}
                  className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                >
                  {currentModels.map((m) => (
                    <option key={m.id} value={m.id}>
                      {m.name} — {m.description}
                    </option>
                  ))}
                </select>
              </div>

              {/* API Key */}
              {currentProvider?.needs_key && (
                <div>
                  <label className="block text-sm font-medium text-txt mb-1">
                    API Key
                    <span className="text-xs text-muted ml-2">{t('settings.keyHint')}</span>
                  </label>
                  <div className="relative">
                    <input
                      type={showKey ? 'text' : 'password'}
                      value={apiKey}
                      onChange={(e) => { setApiKey(e.target.value); setSuccess(false); }}
                      placeholder={t('settings.keyPlaceholder')}
                      className="w-full px-3 py-2 pr-10 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                    />
                    <button
                      type="button"
                      onClick={() => setShowKey(!showKey)}
                      className="absolute right-2 top-1/2 -translate-y-1/2 text-muted hover:text-muted"
                    >
                      {showKey ? <EyeSlashIcon className="w-4 h-4" /> : <EyeIcon className="w-4 h-4" />}
                    </button>
                  </div>
                </div>
              )}

              {/* Base URL */}
              {currentProvider?.needs_url && (
                <div>
                  <label className="block text-sm font-medium text-txt mb-1">
                    Base URL
                  </label>
                  <input
                    type="text"
                    value={baseUrl}
                    onChange={(e) => { setBaseUrl(e.target.value); setSuccess(false); }}
                    placeholder="http://localhost:11434"
                    className="w-full px-3 py-2 rounded-lg border border-line bg-surface text-txt text-sm focus:ring-2 focus:ring-magma/20 focus:border-magma"
                  />
                </div>
              )}

              {/* Error */}
              {error && (
                <div className="text-sm text-red-600 dark:text-red-400 bg-red-50 dark:bg-red-900/20 px-3 py-2 rounded-lg">
                  {error}
                </div>
              )}

              {/* Success */}
              {success && (
                <div className="text-sm text-ok bg-ok/10 px-3 py-2 rounded-lg flex items-center gap-1">
                  <CheckIcon className="w-4 h-4" />
                  {t('settings.saved')}
                </div>
              )}
            </>
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-end gap-3 px-5 py-3 border-t border-line">
          <button
            onClick={onClose}
            className="px-4 py-2 text-sm rounded-lg border border-line text-muted hover:bg-surface2 "
          >
            {t('namespace.cancel')}
          </button>
          <button
            onClick={handleSave}
            disabled={saving || loading}
            className="px-4 py-2 text-sm rounded-lg bg-cortrix-600 text-white hover:bg-cortrix-700 disabled:opacity-50 flex items-center gap-1"
          >
            {saving && <ArrowPathIcon className="w-4 h-4 animate-spin" />}
            {t('settings.save')}
          </button>
        </div>
      </div>
    </div>
  );
}
