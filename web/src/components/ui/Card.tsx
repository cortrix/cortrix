import type { HTMLAttributes, ReactNode } from 'react';

// Cortrix VI Card — rounded-lg + shadow-sm (hover:shadow-md), p-4 / p-6
// padding, with optional Header / Body / Footer sub-sections (VI).

export interface CardProps extends HTMLAttributes<HTMLDivElement> {
  padding?: 'sm' | 'base';
  hoverable?: boolean;
  children?: ReactNode;
}

export function Card({
  padding = 'base',
  hoverable = false,
  className = '',
  children,
  ...rest
}: CardProps) {
  return (
    <div
      className={[
        'rounded-xl border border-line bg-surface card-shadow transition-all duration-200',
        hoverable ? 'hover:border-magma/40' : '',
        padding === 'sm' ? 'p-4' : 'p-6',
        className,
      ].join(' ')}
      {...rest}
    >
      {children}
    </div>
  );
}

export function CardHeader({ className = '', children, ...rest }: HTMLAttributes<HTMLDivElement>) {
  return (
    <div className={`mb-4 flex items-center justify-between gap-3 ${className}`} {...rest}>
      {children}
    </div>
  );
}

export function CardTitle({ className = '', children, ...rest }: HTMLAttributes<HTMLHeadingElement>) {
  return (
    <h3 className={`text-2xl font-semibold text-txt ${className}`} {...rest}>
      {children}
    </h3>
  );
}

export function CardBody({ className = '', children, ...rest }: HTMLAttributes<HTMLDivElement>) {
  return (
    <div className={`text-sm text-txt ${className}`} {...rest}>
      {children}
    </div>
  );
}

export function CardFooter({ className = '', children, ...rest }: HTMLAttributes<HTMLDivElement>) {
  return (
    <div className={`mt-4 flex items-center justify-end gap-3 ${className}`} {...rest}>
      {children}
    </div>
  );
}
