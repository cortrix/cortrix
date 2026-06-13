import { CheckIcon, ArrowPathIcon, ExclamationCircleIcon, CloudArrowUpIcon, ClockIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import type { UploadFile } from '../../types/state';
import { formatFileSize } from '../../utils/formatters';

interface FileItemProps {
  file: UploadFile;
}

export function FileItem({ file }: FileItemProps) {
  const { t } = useTranslation();

  const statusConfig = {
    pending: { icon: ClockIcon, color: 'bg-surface2', iconColor: 'text-muted', label: t('upload.statusPending'), badgeBg: 'bg-surface2 text-muted' },
    uploading: { icon: CloudArrowUpIcon, color: 'bg-magma/15', iconColor: 'text-magma-h', label: t('upload.statusUploading'), badgeBg: 'bg-magma/15 text-magma-h' },
    processing: { icon: ArrowPathIcon, color: 'bg-magma/15', iconColor: 'text-magma-h', label: t('upload.statusProcessing'), badgeBg: 'bg-magma/15 text-magma-h' },
    ready: { icon: CheckIcon, color: 'bg-ok/10', iconColor: 'text-ok', label: t('upload.statusCompleted'), badgeBg: 'bg-ok/10 text-ok' },
    error: { icon: ExclamationCircleIcon, color: 'bg-error/10', iconColor: 'text-error', label: t('upload.statusError'), badgeBg: 'bg-error/10 text-error' },
  };

  const cfg = statusConfig[file.status];
  const Icon = cfg.icon;
  const showProgress = file.status === 'uploading' || file.status === 'processing';
  const isSpinning = file.status === 'processing' || file.status === 'uploading';

  return (
    <div className={`rounded-lg p-4 flex items-center gap-4 bg-surface border card-shadow ${
      file.status === 'error' ? 'border-error/40' : 'border-line'
    }`}>
      <div className={`w-10 h-10 rounded-lg flex items-center justify-center shrink-0 ${cfg.color}`}>
        <Icon className={`w-5 h-5 ${cfg.iconColor} ${isSpinning ? 'animate-spin' : ''}`} />
      </div>
      <div className="flex-1 min-w-0">
        <p className="font-medium text-sm truncate text-txt">{file.file.name}</p>
        {showProgress && (
          <div className="mt-1.5 flex items-center gap-2">
            <div className="flex-1 h-1.5 rounded-full overflow-hidden bg-surface2">
              <div
                className="h-full rounded-full magma-grad transition-all duration-300"
                style={{ width: `${file.progress}%` }}
              />
            </div>
            <span className="text-xs text-muted">{file.progress}%</span>
          </div>
        )}
        {file.status === 'ready' && (
          <p className="text-xs text-muted">
            {formatFileSize(file.file.size)}
          </p>
        )}
        {file.error && (
          <p className="text-xs text-error">{file.error}</p>
        )}
      </div>
      <span className={`px-2.5 py-1 text-xs rounded-full font-medium ${cfg.badgeBg}`}>
        {cfg.label}
      </span>
    </div>
  );
}
