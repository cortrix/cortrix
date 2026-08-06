import {
  Dialog,
  DialogPanel,
  DialogTitle,
  DialogBackdrop,
} from '@headlessui/react';
import { XMarkIcon } from '@heroicons/react/24/outline';
import type { ReactNode } from 'react';

// Cortrix VI Modal — built on @headlessui/react Dialog, backdrop blur +
// fade-in transition, max-w-2xl default (VI rounded-lg / shadow-lg).

export type ModalSize = 'sm' | 'base' | 'lg';

const SIZE_CLASSES: Record<ModalSize, string> = {
  sm: 'max-w-md',
  base: 'max-w-2xl',
  lg: 'max-w-4xl',
};

export interface ModalProps {
  open: boolean;
  onClose: () => void;
  title?: ReactNode;
  size?: ModalSize;
  children?: ReactNode;
  footer?: ReactNode;
}

export function Modal({ open, onClose, title, size = 'base', children, footer }: ModalProps) {
  return (
    <Dialog open={open} onClose={onClose} className="relative z-50">
      <DialogBackdrop
        transition
        className="fixed inset-0 bg-black/40 backdrop-blur-sm transition-opacity duration-200 data-[closed]:opacity-0"
      />
      <div className="fixed inset-0 flex items-center justify-center p-4">
        <DialogPanel
          transition
          className={[
            'w-full rounded-xl bg-surface shadow-lg border border-line',
            'transition-all duration-200 data-[closed]:opacity-0 data-[closed]:scale-95',
            SIZE_CLASSES[size],
          ].join(' ')}
        >
          {title && (
            <div className="flex items-center justify-between border-b border-line px-5 py-4">
              <DialogTitle className="text-lg font-semibold text-txt">
                {title}
              </DialogTitle>
              <button
                type="button"
                onClick={onClose}
                aria-label="Close"
                className="rounded-md p-1 text-muted transition-all duration-200 hover:bg-surface2 hover:text-txt"
              >
                <XMarkIcon className="w-5 h-5" />
              </button>
            </div>
          )}
          <div className="px-5 py-4 text-txt">{children}</div>
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
