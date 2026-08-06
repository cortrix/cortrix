import { Listbox, ListboxButton, ListboxOptions, ListboxOption } from '@headlessui/react';
import { ChevronUpDownIcon, CheckIcon } from '@heroicons/react/24/outline';
import type { ReactNode } from 'react';

// Cortrix VI Select — single-select built on @headlessui/react Listbox,
// brand-colored with rounded + shadowed dropdown (VI).

export interface SelectOption<T extends string = string> {
  value: T;
  label: ReactNode;
}

export interface SelectProps<T extends string = string> {
  value: T;
  onChange: (value: T) => void;
  options: SelectOption<T>[];
  label?: ReactNode;
  placeholder?: string;
  disabled?: boolean;
  id?: string;
  className?: string;
}

export function Select<T extends string = string>({
  value,
  onChange,
  options,
  label,
  placeholder = 'Select…',
  disabled,
  id,
  className = '',
}: SelectProps<T>) {
  const selected = options.find((o) => o.value === value);
  return (
    <div className={`w-full ${className}`}>
      {label && (
        <label htmlFor={id} className="block text-xs font-medium text-muted mb-1">
          {label}
        </label>
      )}
      <Listbox value={value} onChange={onChange} disabled={disabled}>
        <div className="relative">
          <ListboxButton
            id={id}
            className={[
              'relative w-full cursor-default rounded-md border border-line bg-surface py-2 pl-3 pr-9 text-left text-sm',
              'text-txt transition-all duration-200 outline-none',
              'focus:border-magma focus:ring-2 focus:ring-magma/20',
              'disabled:opacity-50 disabled:cursor-not-allowed',
            ].join(' ')}
          >
            <span className="block truncate">{selected ? selected.label : placeholder}</span>
            <ChevronUpDownIcon className="pointer-events-none absolute right-2.5 top-1/2 -translate-y-1/2 w-4 h-4 text-muted" aria-hidden="true" />
          </ListboxButton>
          <ListboxOptions
            anchor="bottom"
            className={[
              'z-50 mt-1 max-h-60 w-[var(--button-width)] overflow-auto rounded-md border border-line bg-surface py-1 text-sm shadow-md',
              'focus:outline-none',
            ].join(' ')}
          >
            {options.map((opt) => (
              <ListboxOption
                key={opt.value}
                value={opt.value}
                className="group flex cursor-default items-center justify-between gap-2 px-3 py-2 text-txt data-[focus]:bg-magma/10 data-[focus]:text-magma-h"
              >
                <span className="truncate">{opt.label}</span>
                <CheckIcon className="w-4 h-4 text-magma opacity-0 group-data-[selected]:opacity-100" aria-hidden="true" />
              </ListboxOption>
            ))}
          </ListboxOptions>
        </div>
      </Listbox>
    </div>
  );
}
