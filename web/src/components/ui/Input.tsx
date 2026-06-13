import { forwardRef } from 'react';
import type { InputHTMLAttributes, TextareaHTMLAttributes, ReactNode } from 'react';

// Cortrix VI § 6.2 Input — text / textarea variants with error + focus states.
// (select / number reuse the native element styled by the same class set; the
// dedicated Select component lives in Select.tsx.)

const BASE_FIELD =
  'w-full rounded-md border bg-surface px-3 py-2 text-sm text-txt placeholder:text-muted ' +
  'transition-all duration-200 outline-none ' +
  'disabled:opacity-50 disabled:cursor-not-allowed';

const FOCUS_OK = 'border-line focus:border-magma focus:ring-2 focus:ring-magma/20';
const FOCUS_ERR = 'border-error focus:border-error focus:ring-2 focus:ring-error/30';

export interface InputProps extends InputHTMLAttributes<HTMLInputElement> {
  label?: ReactNode;
  error?: string;
}

export const Input = forwardRef<HTMLInputElement, InputProps>(function Input(
  { label, error, id, className = '', ...rest },
  ref,
) {
  return (
    <div className="w-full">
      {label && (
        <label htmlFor={id} className="block text-xs font-medium text-muted mb-1">
          {label}
        </label>
      )}
      <input
        ref={ref}
        id={id}
        aria-invalid={error ? true : undefined}
        className={[BASE_FIELD, error ? FOCUS_ERR : FOCUS_OK, className].join(' ')}
        {...rest}
      />
      {error && <p className="mt-1 text-sm text-error">{error}</p>}
    </div>
  );
});

export interface TextareaProps extends TextareaHTMLAttributes<HTMLTextAreaElement> {
  label?: ReactNode;
  error?: string;
}

export const Textarea = forwardRef<HTMLTextAreaElement, TextareaProps>(function Textarea(
  { label, error, id, className = '', ...rest },
  ref,
) {
  return (
    <div className="w-full">
      {label && (
        <label htmlFor={id} className="block text-xs font-medium text-muted mb-1">
          {label}
        </label>
      )}
      <textarea
        ref={ref}
        id={id}
        aria-invalid={error ? true : undefined}
        className={[BASE_FIELD, error ? FOCUS_ERR : FOCUS_OK, className].join(' ')}
        {...rest}
      />
      {error && <p className="mt-1 text-sm text-error">{error}</p>}
    </div>
  );
});
