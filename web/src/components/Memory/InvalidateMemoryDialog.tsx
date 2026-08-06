import { useState } from 'react';
import { ExclamationTriangleIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { MemoryItem } from '../../types/api';
import { Modal, Button } from '../ui';

// Invalidate (soft-delete) confirmation (web UI design — second confirm).
// Memory transparency: this is NOT a hard delete — the memory is marked
// status=invalidated and stays in the transparency history.

interface InvalidateMemoryDialogProps {
  open: boolean;
  memory: MemoryItem | null;
  onClose: () => void;
  onConfirm: (m: MemoryItem) => Promise<void>;
}

export function InvalidateMemoryDialog({
  open,
  memory,
  onClose,
  onConfirm,
}: InvalidateMemoryDialogProps) {
  const { t } = useTranslation();
  const [busy, setBusy] = useState(false);

  const handleConfirm = async () => {
    if (!memory) return;
    setBusy(true);
    try {
      await onConfirm(memory);
      onClose();
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      open={open}
      onClose={onClose}
      size="sm"
      title={t('memory.invalidateTitle')}
      footer={
        <>
          <Button variant="secondary" onClick={onClose} disabled={busy}>
            {t('common.cancel')}
          </Button>
          <Button
            variant="danger"
            onClick={handleConfirm}
            loading={busy}
            data-testid="memory-invalidate-confirm"
          >
            {t('memory.invalidateConfirm')}
          </Button>
        </>
      }
    >
      <div className="flex items-start gap-3">
        <ExclamationTriangleIcon className="mt-0.5 h-6 w-6 shrink-0 text-amber" aria-hidden="true" />
        <div className="space-y-2 text-sm text-txt">
          <p>{t('memory.invalidateWarning')}</p>
          {memory && (
            <p className="rounded-md bg-surface2 px-3 py-2 font-mono text-xs text-muted">
              {memory.memory_id} · {memory.content.slice(0, 80)}
              {memory.content.length > 80 ? '…' : ''}
            </p>
          )}
        </div>
      </div>
    </Modal>
  );
}
