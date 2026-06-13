import type { ButtonHTMLAttributes, ReactNode } from 'react';
import { ArrowPathIcon } from '@heroicons/react/24/outline';

// Cortrix VI § 6.1 Button — primary / secondary / danger variants, sm/base/lg
// sizes, with default/hover/active/disabled/loading states. Magma VI: primary
// uses the `magma-grad` gradient, secondary uses the surface/line tokens
// (SoT: design/vi-mockup/cortrix-vi-console.html).

export type ButtonVariant = 'primary' | 'secondary' | 'danger';
export type ButtonSize = 'sm' | 'base' | 'lg';

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  loading?: boolean;
  leftIcon?: ReactNode;
  children?: ReactNode;
}

const VARIANT_CLASSES: Record<ButtonVariant, string> = {
  primary:
    'magma-grad text-white shadow-lg shadow-magma/25 hover:brightness-110 active:brightness-95 ' +
    'focus-visible:ring-2 focus-visible:ring-magma/40',
  secondary:
    'bg-surface text-txt border border-line hover:border-magma/40 hover:text-magma-h ' +
    'focus-visible:ring-2 focus-visible:ring-magma/30',
  danger:
    'bg-error text-white hover:brightness-95 active:brightness-90 ' +
    'focus-visible:ring-2 focus-visible:ring-error/40',
};

const SIZE_CLASSES: Record<ButtonSize, string> = {
  sm: 'h-8 px-3 text-sm gap-1.5',
  base: 'h-10 px-4 text-sm gap-2',
  lg: 'h-12 px-6 text-base gap-2',
};

export function Button({
  variant = 'primary',
  size = 'base',
  loading = false,
  leftIcon,
  disabled,
  className = '',
  children,
  ...rest
}: ButtonProps) {
  const isDisabled = disabled || loading;
  return (
    <button
      type="button"
      disabled={isDisabled}
      aria-busy={loading || undefined}
      className={[
        'inline-flex items-center justify-center rounded-md font-medium',
        'transition-all duration-200 outline-none focus-visible:ring-offset-0',
        'disabled:opacity-50 disabled:cursor-not-allowed whitespace-nowrap',
        VARIANT_CLASSES[variant],
        SIZE_CLASSES[size],
        className,
      ].join(' ')}
      {...rest}
    >
      {loading ? (
        <ArrowPathIcon className="w-4 h-4 animate-spin" aria-hidden="true" />
      ) : (
        leftIcon
      )}
      {children}
    </button>
  );
}
