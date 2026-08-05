import { useEffect, useState } from 'react';
import { ExclamationTriangleIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { NamespaceDetail } from '../../types/api';
import { Modal, Button, Input } from '../ui';

// Delete confirmation (web UI design § 8.6) — irreversible-from-UI warning + 30-day
// soft-delete note (catalog status='deleted'), gated behind typing the namespace
// name to confirm.

interface DeleteNamespaceDialogProps {
  open: boolean;
  namespace: NamespaceDetail | null;
  onClose: () => void;
  onConfirm: (ns: NamespaceDetail) => Promise<void>;
}

export function DeleteNamespaceDialog({
  open,
  namespace,
  onClose,
  onConfirm,
}: DeleteNamespaceDialogProps) {
  const { t } = useTranslation();
  const [confirmText, setConfirmText] = useState('');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (open) {
      setConfirmText('');
      setBusy(false);
    }
  }, [open]);

  const matches = namespace ? confirmText === namespace.name : false;

  const handleConfirm = async () => {
    if (!namespace || !matches) return;
    setBusy(true);
    try {
      await onConfirm(namespace);
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
      title={t('namespace.deleteTitle')}
      footer={
        <>
          <Button variant="secondary" onClick={onClose} disabled={busy}>
            {t('common.cancel')}
          </Button>
          <Button
            variant="danger"
            onClick={handleConfirm}
            disabled={!matches}
            loading={busy}
            data-testid="namespace-delete-confirm"
          >
            {t('namespace.deleteConfirm')}
          </Button>
        </>
      }
    >
      <div className="space-y-3">
        <div className="flex items-start gap-3">
          <ExclamationTriangleIcon
            className="mt-0.5 h-6 w-6 shrink-0 text-error"
            aria-hidden="true"
          />
          <p className="text-sm text-txt">{t('namespace.deleteWarning')}</p>
        </div>
        <Input
          label={t('namespace.deleteConfirmLabel')}
          value={confirmText}
          onChange={(e) => setConfirmText(e.target.value)}
          placeholder={namespace?.name}
          className="font-mono"
          error={confirmText && !matches ? t('namespace.deleteMismatch') : undefined}
          data-testid="namespace-delete-input"
        />
      </div>
    </Modal>
  );
}
