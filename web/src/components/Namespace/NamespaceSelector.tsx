import { useState, useRef, useEffect } from 'react';
import { ChevronDownIcon, PlusIcon, TrashIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { useAppStore } from '../../store/useAppStore';
import { createNamespace, deleteNamespace } from '../../api/namespaces';

export function NamespaceSelector() {
  const { currentNamespace, setCurrentNamespace, namespaces, loadNamespaces } = useAppStore();
  const [open, setOpen] = useState(false);
  const [creating, setCreating] = useState(false);
  const [newName, setNewName] = useState('');
  const ref = useRef<HTMLDivElement>(null);
  const { t } = useTranslation();

  // Close on outside click
  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) {
        setOpen(false);
        setCreating(false);
      }
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, []);

  const handleCreate = async () => {
    if (!newName.trim()) return;
    await createNamespace(newName.trim());
    await loadNamespaces();
    setCurrentNamespace(newName.trim());
    setNewName('');
    setCreating(false);
  };

  const handleDelete = async (name: string) => {
    if (name === 'default') return;
    await deleteNamespace(name);
    if (currentNamespace === name) setCurrentNamespace('default');
    await loadNamespaces();
  };

  return (
    <div className="relative" ref={ref}>
      <button
        onClick={() => setOpen(!open)}
        className="flex items-center gap-2 px-3 py-1.5 rounded-md text-sm transition-colors border border-line hover:border-magma/40 bg-surface text-txt"
      >
        <span className="w-2 h-2 rounded-full bg-ok" />
        <span className="font-medium">{currentNamespace}</span>
        <ChevronDownIcon className="w-4 h-4 text-muted" />
      </button>

      {open && (
        <div className="absolute top-full left-0 mt-1 w-64 rounded-lg shadow-lg border bg-surface border-line z-50">
          <div className="py-1">
            {namespaces.map((ns) => (
              <div
                key={ns.name}
                className={`flex items-center justify-between px-3 py-2 text-sm cursor-pointer hover:bg-surface2 ${
                  ns.name === currentNamespace ? 'bg-cortrix-50 text-cortrix-700 dark:bg-cortrix-800/20 dark:text-cortrix-400' : 'text-txt'
                }`}
              >
                <span
                  className="flex-1"
                  onClick={() => {
                    setCurrentNamespace(ns.name);
                    setOpen(false);
                  }}
                >
                  {ns.name}
                  <span className="ml-2 text-xs text-muted">
                    {t('namespace.docs', { count: ns.doc_count })}
                  </span>
                </span>
                {ns.name !== 'default' && (
                  <button
                    onClick={(e) => {
                      e.stopPropagation();
                      handleDelete(ns.name);
                    }}
                    className="p-1 rounded hover:bg-red-50 text-muted hover:text-red-500 dark:hover:bg-red-900/20"
                  >
                    <TrashIcon className="w-3.5 h-3.5" />
                  </button>
                )}
              </div>
            ))}
          </div>

          <div className="border-t border-line p-2">
            {creating ? (
              <div className="flex gap-2">
                <input
                  autoFocus
                  value={newName}
                  onChange={(e) => setNewName(e.target.value)}
                  onKeyDown={(e) => e.key === 'Enter' && handleCreate()}
                  placeholder={t('namespace.placeholder')}
                  className="flex-1 px-2 py-1 text-sm rounded border border-line outline-none focus:border-magma bg-surface text-txt"
                />
                <button
                  onClick={handleCreate}
                  className="px-2 py-1 text-xs rounded bg-cortrix-600 text-white hover:bg-cortrix-700"
                >
                  {t('namespace.add')}
                </button>
              </div>
            ) : (
              <button
                onClick={() => setCreating(true)}
                className="w-full flex items-center gap-2 px-2 py-1.5 text-sm text-muted hover:text-magma-h dark:hover:text-magma-h"
              >
                <PlusIcon className="w-4 h-4" />
                {t('namespace.newNamespace')}
              </button>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
