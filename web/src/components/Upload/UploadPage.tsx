import { useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { UploadZone } from './UploadZone';
import { FileList } from './FileList';
import { DocumentList } from './DocumentList';
import { BulkSubmitPanel } from './BulkSubmitPanel';
import { useUploadStore } from '../../store/useUploadStore';
import { useAppStore } from '../../store/useAppStore';
import { ProgrammaticBanner } from '../Common/ProgrammaticBanner';

export function UploadPage() {
  const { t } = useTranslation();
  const { loadDocuments } = useUploadStore();
  const currentNamespace = useAppStore((s) => s.currentNamespace);
  const namespaceReady = useAppStore((s) =>
    s.namespaces.some((ns) => ns.name === s.currentNamespace),
  );

  useEffect(() => {
    if (!namespaceReady) return;
    loadDocuments();
  }, [loadDocuments, currentNamespace, namespaceReady]);

  return (
    <div className="p-6 max-w-4xl mx-auto">
      <div className="mb-6">
        <h1 className="text-2xl font-extrabold tracking-tight text-txt">{t('upload.title')}</h1>
        <p className="text-sm mt-1 text-muted">
          {t('upload.subtitle')}
        </p>
      </div>

      <ProgrammaticBanner
        comment={t('upload.sdkComment')}
        snippet={
          <>
            <span className="text-magma-h">client</span>.<span className="text-amber">documents</span>.
            <span className="text-amber">submit</span>(<span className="text-txt">path</span>=
            <span className="text-ok">&quot;./report.pdf&quot;</span>)
          </>
        }
        className="mb-6"
      />

      <UploadZone />
      <FileList />
      <BulkSubmitPanel />
      <DocumentList />
    </div>
  );
}
