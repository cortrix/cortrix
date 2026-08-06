import { useId, useState, useRef } from 'react';
import type { ReactNode } from 'react';

// Cortrix VI Tooltip — top/bottom/left/right placement, 200ms show delay
// (VI). Self-implemented hover tooltip (headlessui Popover is click-driven
// and unsuited to hover hints); accessible via aria-describedby.

export type TooltipPlacement = 'top' | 'bottom' | 'left' | 'right';

const PLACEMENT_CLASSES: Record<TooltipPlacement, string> = {
  top: 'bottom-full left-1/2 -translate-x-1/2 mb-2',
  bottom: 'top-full left-1/2 -translate-x-1/2 mt-2',
  left: 'right-full top-1/2 -translate-y-1/2 mr-2',
  right: 'left-full top-1/2 -translate-y-1/2 ml-2',
};

export interface TooltipProps {
  content: ReactNode;
  placement?: TooltipPlacement;
  children: ReactNode;
  delayMs?: number;
}

export function Tooltip({ content, placement = 'top', children, delayMs = 200 }: TooltipProps) {
  const id = useId();
  const [open, setOpen] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const show = () => {
    timer.current = setTimeout(() => setOpen(true), delayMs);
  };
  const hide = () => {
    if (timer.current) clearTimeout(timer.current);
    setOpen(false);
  };

  return (
    <span
      className="relative inline-flex"
      onMouseEnter={show}
      onMouseLeave={hide}
      onFocus={show}
      onBlur={hide}
      aria-describedby={open ? id : undefined}
    >
      {children}
      {open && (
        <span
          id={id}
          role="tooltip"
          className={[
            'absolute z-50 whitespace-nowrap rounded-md bg-txt px-2 py-1 text-xs font-medium text-bg shadow-md',
            'transition-all duration-200',
            PLACEMENT_CLASSES[placement],
          ].join(' ')}
        >
          {content}
        </span>
      )}
    </span>
  );
}
