import { useEffect, useMemo, useState } from 'react';
import { TabGroup, TabList, Tab, TabPanels, TabPanel } from '@headlessui/react';
import { useTranslation } from 'react-i18next';
import type {
  NamespaceConfigKey,
  NamespaceConfigs,
  NamespaceDetail,
  NamespaceIsolationMode,
  NamespaceVisibility,
} from '../../types/api';
import { Modal, Button, Input, Textarea, Select } from '../ui';
import { validateNamespace } from '../../utils/validators';
import { ConfigSection } from './ConfigSection';
import { JsonEditor } from './JsonEditor';
import { CONFIG_KEYS, configsForTab, type ConfigTab } from './namespaceConfigMeta';

// Namespace create / edit dialog (P02a design § 8.1-8.3). 5 tabs (§ 8.2):
// Basic / Retrieval / Processing / Memory / Advanced. Each *_config is a JSON
// blob edited via ConfigSection (Monaco); Advanced also exposes a single Raw
// JSON editor over all 11 configs (Hybrid Form / JSON, § 8.2). Invalid JSON in
// any block blocks the save.

type ConfigTextMap = Record<NamespaceConfigKey, string>;

const ISOLATION_OPTIONS: { value: NamespaceIsolationMode; label: string }[] = [
  { value: 'shared', label: 'shared' },
  { value: 'isolated', label: 'isolated' },
];
const VISIBILITY_OPTIONS: { value: NamespaceVisibility; label: string }[] = [
  { value: 'tenant', label: 'tenant' },
  { value: 'private', label: 'private' },
  { value: 'public', label: 'public' },
];

function emptyTextMap(): ConfigTextMap {
  return CONFIG_KEYS.reduce((acc, k) => {
    acc[k] = '{}';
    return acc;
  }, {} as ConfigTextMap);
}

function configsToTextMap(configs?: NamespaceConfigs): ConfigTextMap {
  const map = emptyTextMap();
  if (!configs) return map;
  for (const k of CONFIG_KEYS) {
    const v = configs[k];
    map[k] = v && Object.keys(v).length > 0 ? JSON.stringify(v, null, 2) : '{}';
  }
  return map;
}

/** Parse a text map → { configs, errors }. errors keyed by config key. */
function parseTextMap(map: ConfigTextMap): {
  configs: Partial<NamespaceConfigs>;
  errors: Partial<Record<NamespaceConfigKey, string>>;
} {
  const configs: Partial<NamespaceConfigs> = {};
  const errors: Partial<Record<NamespaceConfigKey, string>> = {};
  for (const k of CONFIG_KEYS) {
    const text = map[k].trim();
    if (text === '' || text === '{}') {
      configs[k] = {};
      continue;
    }
    try {
      const parsed = JSON.parse(text);
      if (typeof parsed !== 'object' || Array.isArray(parsed) || parsed === null) {
        errors[k] = 'Must be a JSON object.';
      } else {
        configs[k] = parsed as Record<string, unknown>;
      }
    } catch {
      errors[k] = 'Invalid JSON.';
    }
  }
  return { configs, errors };
}

export interface NamespaceFormValues {
  name: string;
  description: string;
  isolation_mode: NamespaceIsolationMode;
  visibility: NamespaceVisibility;
  configs: Partial<NamespaceConfigs>;
}

interface NamespaceFormDialogProps {
  open: boolean;
  onClose: () => void;
  /** When set, the dialog is in edit mode (name is read-only). */
  namespace?: NamespaceDetail | null;
  onSubmit: (values: NamespaceFormValues) => Promise<void>;
}

const TABS: { key: 'basic' | ConfigTab; labelKey: string }[] = [
  { key: 'basic', labelKey: 'namespace.tabBasic' },
  { key: 'retrieval', labelKey: 'namespace.tabRetrieval' },
  { key: 'processing', labelKey: 'namespace.tabProcessing' },
  { key: 'memory', labelKey: 'namespace.tabMemory' },
  { key: 'advanced', labelKey: 'namespace.tabAdvanced' },
];

export function NamespaceFormDialog({
  open,
  onClose,
  namespace,
  onSubmit,
}: NamespaceFormDialogProps) {
  const { t } = useTranslation();
  const isEdit = Boolean(namespace);

  const [name, setName] = useState('');
  const [description, setDescription] = useState('');
  const [isolation, setIsolation] = useState<NamespaceIsolationMode>('shared');
  const [visibility, setVisibility] = useState<NamespaceVisibility>('tenant');
  const [configText, setConfigText] = useState<ConfigTextMap>(emptyTextMap);
  const [nameError, setNameError] = useState<string | null>(null);
  const [formError, setFormError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (open) {
      setName(namespace?.name ?? '');
      setDescription(namespace?.description ?? '');
      setIsolation(namespace?.isolation_mode ?? 'shared');
      setVisibility(namespace?.visibility ?? 'tenant');
      setConfigText(configsToTextMap(namespace?.configs));
      setNameError(null);
      setFormError(null);
      setSaving(false);
    }
  }, [open, namespace]);

  const { errors: configErrors } = useMemo(() => parseTextMap(configText), [configText]);
  const hasConfigErrors = Object.keys(configErrors).length > 0;

  // Raw JSON over all 11 configs (Advanced tab). Built from the per-config map;
  // edits are re-distributed back to each key when the whole object parses.
  const rawJson = useMemo(() => {
    const obj: Record<string, unknown> = {};
    for (const k of CONFIG_KEYS) {
      try {
        obj[k] = JSON.parse(configText[k] || '{}');
      } catch {
        obj[k] = configText[k]; // keep raw text so the user sees their edit
      }
    }
    return JSON.stringify(obj, null, 2);
  }, [configText]);
  const [rawError, setRawError] = useState<string | null>(null);

  const handleRawChange = (text: string) => {
    setRawError(null);
    try {
      const parsed = JSON.parse(text);
      if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
        setRawError('Must be a JSON object.');
        return;
      }
      setConfigText((prev) => {
        const next = { ...prev };
        for (const k of CONFIG_KEYS) {
          if (k in parsed) {
            next[k] = JSON.stringify((parsed as Record<string, unknown>)[k], null, 2);
          }
        }
        return next;
      });
    } catch {
      setRawError('Invalid JSON.');
    }
  };

  const setConfig = (key: NamespaceConfigKey, text: string) =>
    setConfigText((prev) => ({ ...prev, [key]: text }));

  const handleSubmit = async () => {
    setFormError(null);
    if (!isEdit) {
      const v = validateNamespace(name);
      if (!v.valid) {
        setNameError(v.error);
        return;
      }
    }
    const { configs, errors } = parseTextMap(configText);
    if (Object.keys(errors).length > 0) {
      setFormError(t('namespace.invalidJson'));
      return;
    }
    setSaving(true);
    try {
      await onSubmit({
        name: name.trim(),
        description: description.trim(),
        isolation_mode: isolation,
        visibility,
        configs,
      });
      onClose();
    } catch (e) {
      setFormError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal
      open={open}
      onClose={onClose}
      size="lg"
      title={isEdit ? t('namespace.editTitle') : t('namespace.createTitle')}
      footer={
        <>
          <Button variant="secondary" onClick={onClose} disabled={saving}>
            {t('common.cancel')}
          </Button>
          <Button
            onClick={handleSubmit}
            loading={saving}
            disabled={hasConfigErrors || Boolean(rawError)}
            data-testid="namespace-form-submit"
          >
            {isEdit ? t('common.save') : t('common.create')}
          </Button>
        </>
      }
    >
      <TabGroup>
        <TabList className="mb-4 flex gap-1 border-b border-line">
          {TABS.map((tab) => (
            <Tab
              key={tab.key}
              className="-mb-px border-b-2 border-transparent px-3 py-2 text-sm font-medium text-muted outline-none data-[selected]:border-magma data-[selected]:text-magma-h data-[hover]:text-txt"
            >
              {t(tab.labelKey)}
            </Tab>
          ))}
        </TabList>
        <TabPanels>
          {/* Basic */}
          <TabPanel className="space-y-4">
            <Input
              label={t('namespace.nameLabel')}
              value={name}
              onChange={(e) => {
                setName(e.target.value);
                setNameError(null);
              }}
              placeholder={t('namespace.namePlaceholder')}
              disabled={isEdit}
              error={nameError ?? undefined}
              className="font-mono"
              data-testid="namespace-name-input"
            />
            {!isEdit && <p className="-mt-2 text-xs text-muted">{t('namespace.nameHint')}</p>}
            <Textarea
              label={t('namespace.descriptionLabel')}
              rows={2}
              value={description}
              placeholder={t('namespace.descriptionPlaceholder')}
              onChange={(e) => setDescription(e.target.value)}
            />
            <div className="grid grid-cols-2 gap-4">
              <Select<NamespaceIsolationMode>
                label={t('namespace.isolationMode')}
                value={isolation}
                onChange={setIsolation}
                options={ISOLATION_OPTIONS}
              />
              <Select<NamespaceVisibility>
                label={t('namespace.visibility')}
                value={visibility}
                onChange={setVisibility}
                options={VISIBILITY_OPTIONS}
              />
            </div>
          </TabPanel>

          {/* Retrieval / Processing / Memory share the ConfigSection list */}
          {(['retrieval', 'processing', 'memory'] as ConfigTab[]).map((tab) => (
            <TabPanel key={tab} className="space-y-3">
              {configsForTab(tab).map((meta) => (
                <ConfigSection
                  key={meta.key}
                  meta={meta}
                  value={configText[meta.key]}
                  onChange={(text) => setConfig(meta.key, text)}
                  error={configErrors[meta.key]}
                />
              ))}
            </TabPanel>
          ))}

          {/* Advanced: watcher_config + Raw JSON over all 11 */}
          <TabPanel className="space-y-4">
            {configsForTab('advanced').map((meta) => (
              <ConfigSection
                key={meta.key}
                meta={meta}
                value={configText[meta.key]}
                onChange={(text) => setConfig(meta.key, text)}
                error={configErrors[meta.key]}
              />
            ))}
            <div>
              <div className="mb-1 flex items-center justify-between">
                <h4 className="text-sm font-semibold text-txt">{t('namespace.rawJson')}</h4>
              </div>
              <p className="mb-2 text-xs text-muted">{t('namespace.rawJsonHint')}</p>
              <JsonEditor
                value={rawJson}
                onChange={handleRawChange}
                height={260}
                ariaLabel="All configs raw JSON"
              />
              {rawError && <p className="mt-1 text-xs text-error">{rawError}</p>}
            </div>
          </TabPanel>
        </TabPanels>
      </TabGroup>

      {formError && <p className="mt-4 text-sm text-error">{formError}</p>}
    </Modal>
  );
}
