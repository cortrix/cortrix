import toast, { Toaster, type Toast as HotToast } from 'react-hot-toast';
import {
  CheckCircleIcon,
  ExclamationTriangleIcon,
  XCircleIcon,
  InformationCircleIcon,
  XMarkIcon,
} from '@heroicons/react/24/outline';
import type { ComponentType, SVGProps } from 'react';

// Cortrix VI Toast — built on react-hot-toast, 4 semantic types
// (success / warning / error / info), auto-dismiss 3s, manually closable.

type ToastType = 'success' | 'warning' | 'error' | 'info';

const TYPE_META: Record<
  ToastType,
  { icon: ComponentType<SVGProps<SVGSVGElement>>; iconClass: string; ring: string }
> = {
  success: { icon: CheckCircleIcon, iconClass: 'text-success', ring: 'border-success/40' },
  warning: { icon: ExclamationTriangleIcon, iconClass: 'text-warning', ring: 'border-warning/40' },
  error: { icon: XCircleIcon, iconClass: 'text-error', ring: 'border-error/40' },
  info: { icon: InformationCircleIcon, iconClass: 'text-info', ring: 'border-info/40' },
};

const DEFAULT_DURATION = 3000;

function renderToast(type: ToastType, message: string, t: HotToast) {
  const { icon: Icon, iconClass, ring } = TYPE_META[type];
  return (
    <div
      className={[
        'pointer-events-auto flex items-start gap-3 rounded-md border bg-surface px-4 py-3 shadow-md',
        ring,
        t.visible ? 'opacity-100' : 'opacity-0',
      ].join(' ')}
      role="status"
    >
      <Icon className={`mt-0.5 h-5 w-5 shrink-0 ${iconClass}`} aria-hidden="true" />
      <span className="flex-1 text-sm text-txt">{message}</span>
      <button
        type="button"
        onClick={() => toast.dismiss(t.id)}
        aria-label="Dismiss"
        className="rounded p-0.5 text-muted transition-all duration-200 hover:text-txt"
      >
        <XMarkIcon className="h-4 w-4" />
      </button>
    </div>
  );
}

/** Brand-styled toast helpers. Usage: `notify.success('Saved')`. */
export const notify = {
  success: (message: string) =>
    toast.custom((t) => renderToast('success', message, t), { duration: DEFAULT_DURATION }),
  warning: (message: string) =>
    toast.custom((t) => renderToast('warning', message, t), { duration: DEFAULT_DURATION }),
  error: (message: string) =>
    toast.custom((t) => renderToast('error', message, t), { duration: DEFAULT_DURATION }),
  info: (message: string) =>
    toast.custom((t) => renderToast('info', message, t), { duration: DEFAULT_DURATION }),
  dismiss: (id?: string) => toast.dismiss(id),
};

/** Mount once near the app root to render toasts. */
export function ToastContainer() {
  return <Toaster position="top-right" gutter={8} containerClassName="!z-[60]" />;
}
