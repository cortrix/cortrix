import { Dialog, DialogPanel, DialogTitle, DialogBackdrop } from '@headlessui/react';
import { XMarkIcon } from '@heroicons/react/24/outline';
import type { ReactNode } from 'react';

// Right-anchored slide-over drawer (web UI design § 8.5 NamespaceDetailDrawer) —
// shows detail without losing the list context. Built on @headlessui/react
// Dialog; Magma VI surface/line tokens (works light + dark).

interface DrawerProps {
  open: boolean;
  onClose: () => void;
  title?: ReactNode;
  subtitle?: ReactNode;
  children?: ReactNode;
  footer?: ReactNode;
}

export function Drawer({ open, onClose, title, subtitle, children, footer }: DrawerProps) {
  return (
    <Dialog open={open} onClose={onClose} className="relative z-50">
      <DialogBackdrop
        transition
        className="fixed inset-0 bg-black/40 backdrop-blur-sm transition-opacity duration-200 data-[closed]:opacity-0"
      />
      <div className="fixed inset-0 flex justify-end">
        <DialogPanel
          transition
          className={[
            'flex h-full w-full max-w-xl flex-col bg-surface shadow-lg border-l border-line',
            'transition-transform duration-300 data-[closed]:translate-x-full',
          ].join(' ')}
        >
          <div className="flex items-start justify-between border-b border-line px-5 py-4">
            <div className="min-w-0">
              {title && (
                <DialogTitle className="truncate text-lg font-semibold text-txt">
                  {title}
                </DialogTitle>
              )}
              {subtitle && <div className="mt-0.5 text-xs text-muted">{subtitle}</div>}
            </div>
            <button
              type="button"
              onClick={onClose}
              aria-label="Close"
              className="rounded-md p-1 text-muted transition-all duration-200 hover:bg-surface2 hover:text-txt"
            >
              <XMarkIcon className="h-5 w-5" />
            </button>
          </div>
          <div className="flex-1 overflow-y-auto px-5 py-4">{children}</div>
          {footer && (
            <div className="flex items-center justify-end gap-3 border-t border-line px-5 py-3">
              {footer}
            </div>
          )}
        </DialogPanel>
      </div>
    </Dialog>
  );
}
