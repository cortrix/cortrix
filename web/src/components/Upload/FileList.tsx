import { useTranslation } from 'react-i18next';
import { useUploadStore } from '../../store/useUploadStore';
import { usePolling } from '../../hooks/usePolling';
import { POLL_INTERVAL_MS } from '../../utils/constants';
import { FileItem } from './FileItem';

export function FileList() {
  const files = useUploadStore((s) => s.files);
  const pollProcessing = useUploadStore((s) => s.pollProcessing);
  const { t } = useTranslation();

  const hasProcessing = files.some((f) => f.status === 'processing');
  usePolling(() => pollProcessing(), POLL_INTERVAL_MS, hasProcessing);

  if (files.length === 0) return null;

  return (
    <div className="mt-6 space-y-3">
      <h2 className="text-sm font-semibold uppercase tracking-wider text-muted">
        {t('upload.listTitle', { count: files.length })}
      </h2>
      {files.map((f) => (
        <FileItem key={f.id} file={f} />
      ))}
    </div>
  );
}
