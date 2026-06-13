import { ExclamationCircleIcon, XMarkIcon } from '@heroicons/react/24/outline';

interface ErrorMessageProps {
  message: string;
  onDismiss?: () => void;
}

export function ErrorMessage({ message, onDismiss }: ErrorMessageProps) {
  return (
    <div className="flex items-center gap-3 px-4 py-3 rounded-lg bg-red-50 border border-red-200 text-red-700 dark:bg-red-900/20 dark:border-red-900/40 dark:text-red-400">
      <ExclamationCircleIcon className="w-5 h-5 shrink-0" />
      <span className="text-sm flex-1">{message}</span>
      {onDismiss && (
        <button onClick={onDismiss} className="p-1 hover:bg-red-100 rounded dark:hover:bg-red-900/30">
          <XMarkIcon className="w-4 h-4" />
        </button>
      )}
    </div>
  );
}
