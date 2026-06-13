import { Fragment, useState } from 'react';
import { Dialog, Transition } from '@headlessui/react';
import { XMarkIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';
import { createNamespace } from '../../api/namespaces';
import { validateNamespace } from '../../utils/validators';
import { LoadingSpinner } from '../Common/LoadingSpinner';
import { ErrorMessage } from '../Common/ErrorMessage';

interface Props {
  isOpen: boolean;
  onClose: () => void;
  onSuccess: (name: string) => void;
}

export function CreateNamespaceDialog({ isOpen, onClose, onSuccess }: Props) {
  const [name, setName] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const { t } = useTranslation();

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError(null);

    const validation = validateNamespace(name);
    if (!validation.valid) {
      setError(validation.error);
      return;
    }

    setLoading(true);
    try {
      await createNamespace(name);
      onSuccess(name);
      setName('');
      onClose();
    } catch (err) {
      if (err instanceof Error) {
        if (err.message.includes('409') || err.message.includes('already exists')) {
          setError(t('namespace.alreadyExists'));
        } else {
          setError(err.message);
        }
      } else {
        setError(t('namespace.createFailed'));
      }
    } finally {
      setLoading(false);
    }
  };

  return (
    <Transition appear show={isOpen} as={Fragment}>
      <Dialog as="div" className="relative z-50" onClose={onClose}>
        <Transition.Child
          as={Fragment}
          enter="ease-out duration-300"
          enterFrom="opacity-0"
          enterTo="opacity-100"
          leave="ease-in duration-200"
          leaveFrom="opacity-100"
          leaveTo="opacity-0"
        >
          <div className="fixed inset-0 bg-black bg-opacity-25 dark:bg-opacity-50" />
        </Transition.Child>

        <div className="fixed inset-0 overflow-y-auto">
          <div className="flex min-h-full items-center justify-center p-4">
            <Transition.Child
              as={Fragment}
              enter="ease-out duration-300"
              enterFrom="opacity-0 scale-95"
              enterTo="opacity-100 scale-100"
              leave="ease-in duration-200"
              leaveFrom="opacity-100 scale-100"
              leaveTo="opacity-0 scale-95"
            >
              <Dialog.Panel className="w-full max-w-md transform overflow-hidden rounded-xl bg-surface p-6 shadow-xl transition-all border border-line">
                <div className="flex items-center justify-between mb-4">
                  <Dialog.Title className="text-lg font-semibold text-txt">
                    {t('namespace.createTitle')}
                  </Dialog.Title>
                  <button
                    onClick={onClose}
                    className="p-1 rounded-md hover:bg-surface2 text-muted hover:text-txt"
                  >
                    <XMarkIcon className="w-5 h-5" />
                  </button>
                </div>

                <form onSubmit={handleSubmit}>
                  <div className="mb-4">
                    <label
                      htmlFor="namespace-name"
                      className="block text-sm font-medium mb-2 text-txt"
                    >
                      {t('namespace.nameLabel')}
                    </label>
                    <input
                      id="namespace-name"
                      type="text"
                      value={name}
                      onChange={(e) => setName(e.target.value)}
                      placeholder={t('namespace.namePlaceholder')}
                      className="w-full px-3 py-2 rounded-lg text-sm border border-line bg-surface text-txt placeholder:text-muted focus:ring-2 focus:ring-magma/20 focus:border-magma outline-none"
                      disabled={loading}
                    />
                    <p className="mt-1 text-xs text-muted">
                      {t('namespace.nameHint')}
                    </p>
                  </div>

                  {error && (
                    <div className="mb-4">
                      <ErrorMessage message={error} />
                    </div>
                  )}

                  <div className="flex gap-3">
                    <button
                      type="button"
                      onClick={onClose}
                      className="flex-1 px-4 py-2 rounded-lg text-sm font-medium border border-line hover:bg-surface2 text-txt"
                      disabled={loading}
                    >
                      {t('namespace.cancel')}
                    </button>
                    <button
                      type="submit"
                      className="flex-1 px-4 py-2 rounded-lg text-sm font-medium magma-grad text-white disabled:opacity-50 disabled:cursor-not-allowed flex items-center justify-center gap-2"
                      disabled={loading}
                    >
                      {loading && <LoadingSpinner size="sm" />}
                      {t('namespace.create')}
                    </button>
                  </div>
                </form>
              </Dialog.Panel>
            </Transition.Child>
          </div>
        </div>
      </Dialog>
    </Transition>
  );
}
