import { useCallback } from 'react';
import { useDropzone } from 'react-dropzone';
import { CloudArrowUpIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useUploadStore } from '../../store/useUploadStore';
import { ACCEPTED_TYPES, MAX_FILE_SIZE } from '../../utils/constants';

export function UploadZone() {
  const addFiles = useUploadStore((s) => s.addFiles);
  const { t } = useTranslation();

  const onDrop = useCallback(
    (acceptedFiles: File[]) => {
      if (acceptedFiles.length > 0) addFiles(acceptedFiles);
    },
    [addFiles],
  );

  const { getRootProps, getInputProps, isDragActive } = useDropzone({
    onDrop,
    accept: ACCEPTED_TYPES,
    maxSize: MAX_FILE_SIZE,
  });

  return (
    <div
      {...getRootProps()}
      role="button"
      aria-label={t('upload.dropDefault')}
      data-testid="upload-dropzone"
      className={`border-2 border-dashed rounded-xl p-12 text-center cursor-pointer transition-colors
        ${isDragActive
          ? 'border-cortrix-500 bg-cortrix-50 dark:bg-cortrix-900/10'
          : 'border-line hover:border-magma/40 bg-surface border-line hover:border-magma/40 bg-surface'
        }`}
    >
      <input {...getInputProps()} data-testid="upload-file-input" />
      <CloudArrowUpIcon className="w-12 h-12 mx-auto mb-4 text-muted" />
      <p className="font-medium text-muted text-txt">
        {isDragActive ? t('upload.dropActive') : t('upload.dropDefault')}
      </p>
      <p className="text-sm mt-1 text-muted">{t('upload.browseHint')}</p>
      <p className="text-xs mt-3 text-muted">
        {t('upload.supportedFormats')}
      </p>
    </div>
  );
}
