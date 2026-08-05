import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import type { MemoryItem, MemoryType } from '../../types/api';
import { Modal, Button, Textarea, Select } from '../ui';

// Create / Edit memory dialog (web UI design § 7.1, memory transparency POST + PATCH). One
// component drives both modes — `memory` present = edit. Edit creates a new
// memory and invalidates the old one (§ 7.2 — extraction_method=user_edit), so
// we surface that note. content maxLength 2000 (memory transparency).

const MAX_CONTENT = 2000;

const TYPE_OPTIONS: { value: MemoryType; label: string }[] = [
  { value: 'fact', label: 'fact' },
  { value: 'preference', label: 'preference' },
  { value: 'event', label: 'event' },
];

interface MemoryFormDialogProps {
  open: boolean;
  onClose: () => void;
  /** When set, the dialog is in edit mode. */
  memory?: MemoryItem | null;
  onSubmit: (values: { content: string; memory_type: MemoryType }) => Promise<void>;
}

export function MemoryFormDialog({ open, onClose, memory, onSubmit }: MemoryFormDialogProps) {
  const { t } = useTranslation();
  const isEdit = Boolean(memory);
  const [content, setContent] = useState('');
  const [type, setType] = useState<MemoryType>('fact');
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (open) {
      setContent(memory?.content ?? '');
      setType(memory?.memory_type ?? 'fact');
      setError(null);
      setSaving(false);
    }
  }, [open, memory]);

  const handleSubmit = async () => {
    const trimmed = content.trim();
    if (!trimmed) {
      setError(t('memory.contentRequired'));
      return;
    }
    if (trimmed.length > MAX_CONTENT) {
      setError(t('memory.contentTooLong'));
      return;
    }
    setSaving(true);
    setError(null);
    try {
      await onSubmit({ content: trimmed, memory_type: type });
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <Modal
      open={open}
      onClose={onClose}
      size="base"
      title={isEdit ? t('memory.editTitle') : t('memory.createTitle')}
      footer={
        <>
          <Button variant="secondary" onClick={onClose} disabled={saving}>
            {t('common.cancel')}
          </Button>
          <Button onClick={handleSubmit} loading={saving} data-testid="memory-form-submit">
            {isEdit ? t('common.save') : t('common.create')}
          </Button>
        </>
      }
    >
      <form
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          void handleSubmit();
        }}
      >
        <div>
          <Textarea
            label={t('memory.contentLabel')}
            rows={4}
            value={content}
            maxLength={MAX_CONTENT + 1}
            placeholder={t('memory.contentPlaceholder')}
            onChange={(e) => setContent(e.target.value)}
            error={error ?? undefined}
            data-testid="memory-content-input"
          />
          <p className="mt-1 text-right font-mono text-xs text-muted">
            {content.length}/{MAX_CONTENT}
          </p>
        </div>

        <Select<MemoryType>
          label={t('memory.typeLabel')}
          value={type}
          onChange={setType}
          options={TYPE_OPTIONS}
        />

        {isEdit && (
          <p className="rounded-md border border-line bg-surface2 px-3 py-2 text-xs text-muted">
            {t('memory.editNote')}
          </p>
        )}
      </form>
    </Modal>
  );
}
