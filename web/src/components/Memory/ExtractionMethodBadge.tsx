import {
  SparklesIcon,
  UserIcon,
  PencilIcon,
  CogIcon,
  ArrowDownTrayIcon,
} from '@heroicons/react/24/outline';
import type { ComponentType, SVGProps } from 'react';
import type { ExtractionMethod } from '../../types/api';

// Memory transparency extraction_method 5-enum → Heroicons (web UI design). NO emoji
// (VI SPECS — Heroicons only). Each entry is icon-sm (h-4 w-4) per the VI
// icon token. Magma VI muted/secondary tone so it reads in both themes.

const META: Record<
  ExtractionMethod,
  { icon: ComponentType<SVGProps<SVGSVGElement>>; label: string }
> = {
  llm_extracted: { icon: SparklesIcon, label: 'LLM extracted' },
  user_created: { icon: UserIcon, label: 'User created' },
  user_edit: { icon: PencilIcon, label: 'User edited' },
  system: { icon: CogIcon, label: 'System' },
  imported: { icon: ArrowDownTrayIcon, label: 'Imported' },
};

export function ExtractionMethodBadge({ method }: { method: ExtractionMethod }) {
  const entry = META[method];
  if (!entry) return null;
  const Icon = entry.icon;
  return (
    <span
      className="inline-flex items-center gap-1.5 text-xs text-muted"
      data-extraction-method={method}
    >
      <Icon className="h-4 w-4 text-muted" aria-hidden="true" />
      {entry.label}
    </span>
  );
}
