import type { HTMLAttributes, ReactNode } from 'react';

// Cortrix VI § 6.8 Badge — rounded-full + text-xs status / tag chips. Magma VI
// palette (SoT: design/vi-mockup/cortrix-vi-console.html): magma / amber / ok
// accents. Semantic aliases (primary/success/warning/error) are kept so
// existing call sites map onto the Magma tokens without churn.

export type BadgeVariant =
  | 'magma' | 'amber' | 'ok'
  | 'primary' | 'success' | 'warning' | 'error';

const VARIANT_CLASSES: Record<BadgeVariant, string> = {
  magma: 'bg-magma/15 text-magma-h',
  amber: 'bg-amber/15 text-amber',
  ok: 'bg-ok/10 text-ok',
  // Semantic aliases → Magma palette
  primary: 'bg-magma/15 text-magma-h',
  success: 'bg-ok/10 text-ok',
  warning: 'bg-amber/15 text-amber',
  error: 'bg-error/10 text-error',
};

export interface BadgeProps extends HTMLAttributes<HTMLSpanElement> {
  variant?: BadgeVariant;
  children?: ReactNode;
}

export function Badge({ variant = 'primary', className = '', children, ...rest }: BadgeProps) {
  return (
    <span
      className={[
        'inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs font-medium',
        VARIANT_CLASSES[variant],
        className,
      ].join(' ')}
      {...rest}
    >
      {children}
    </span>
  );
}
