import type { ComponentType, SVGProps } from 'react';
import {
  ArrowTopRightOnSquareIcon,
  CheckCircleIcon,
  EnvelopeIcon,
  LockClosedIcon,
} from '@heroicons/react/24/outline';
import { Badge } from '../ui';

// Unified Enterprise placeholder template (web UI design). A CE
// deployment ships every Ent surface as a marketing placeholder until the
// backend feature flag reports the feature enabled (feature flag store).
// Content: Title + Enterprise badge + Description + Problem solved + Use cases
// + "Coming in <milestone>" badge + 3 marketing CTAs. Heroicons only (no emoji,
// VI SPECS "no emoji icons").

export interface PlaceholderMarketingLinks {
  /** cortrix.ai roadmap sub-page (the cloud roadmap). */
  roadmap: string;
  /** Cloud V1.5 sign-up page (the cloud roadmap). */
  cloudBeta?: string;
  /** mailto: enterprise contact. */
  contact: string;
}

export interface PlaceholderPageProps {
  featureKey: 'text_to_sql' | 'audit_log_ent' | 'multi_tenant_switch';
  icon: ComponentType<SVGProps<SVGSVGElement>>;
  title: string;
  description: string;
  problemSolved: string;
  useCases: string[];
  availableIn: string;
  marketingLinks: PlaceholderMarketingLinks;
}

export function PlaceholderPage({
  featureKey,
  icon: Icon,
  title,
  description,
  problemSolved,
  useCases,
  availableIn,
  marketingLinks,
}: PlaceholderPageProps) {
  return (
    <section
      className="mx-auto max-w-3xl px-6 py-8 space-y-6"
      data-testid="ent-placeholder"
      data-feature={featureKey}
    >
      <header className="space-y-3">
        <div className="flex items-center gap-2">
          <Badge variant="magma" data-testid="ent-badge">
            Enterprise
          </Badge>
          <Badge variant="amber" data-testid="ent-available-in">
            Coming in {availableIn}
          </Badge>
        </div>
        <div className="flex items-center gap-3">
          <span className="grid h-11 w-11 place-items-center rounded-xl bg-magma/10 text-magma-h">
            <Icon className="h-6 w-6" aria-hidden="true" />
          </span>
          <h1 className="text-3xl font-extrabold tracking-tight text-txt">{title}</h1>
        </div>
        <p className="text-sm leading-relaxed text-muted">{description}</p>
      </header>

      <div className="rounded-xl border border-line bg-surface p-5 card-shadow">
        <h2 className="flex items-center gap-2 text-[15px] font-semibold text-txt">
          <LockClosedIcon className="h-5 w-5 text-magma-h" aria-hidden="true" />
          What it solves
        </h2>
        <p className="mt-2 text-sm leading-relaxed text-muted">{problemSolved}</p>
      </div>

      <div className="rounded-xl border border-line bg-surface p-5 card-shadow">
        <h2 className="text-[15px] font-semibold text-txt">Use cases</h2>
        <ul className="mt-3 space-y-2" data-testid="ent-use-cases">
          {useCases.map((useCase) => (
            <li key={useCase} className="flex items-start gap-2.5 text-sm text-txt">
              <CheckCircleIcon className="mt-0.5 h-5 w-5 shrink-0 text-ok" aria-hidden="true" />
              <span>{useCase}</span>
            </li>
          ))}
        </ul>
      </div>

      <div className="flex flex-wrap gap-3" data-testid="ent-cta">
        <a
          href={marketingLinks.roadmap}
          target="_blank"
          rel="noopener noreferrer"
          className="inline-flex items-center gap-2 rounded-lg bg-magma px-4 py-2.5 text-sm font-semibold text-white transition-colors hover:bg-magma-h"
        >
          <ArrowTopRightOnSquareIcon className="h-4 w-4" aria-hidden="true" />
          View roadmap
        </a>
        {marketingLinks.cloudBeta && (
          <a
            href={marketingLinks.cloudBeta}
            target="_blank"
            rel="noopener noreferrer"
            className="inline-flex items-center gap-2 rounded-lg border border-line bg-surface px-4 py-2.5 text-sm font-semibold text-txt transition-colors hover:bg-surface2"
          >
            <ArrowTopRightOnSquareIcon className="h-4 w-4" aria-hidden="true" />
            Join Cloud beta
          </a>
        )}
        <a
          href={marketingLinks.contact}
          className="inline-flex items-center gap-2 rounded-lg border border-line bg-surface px-4 py-2.5 text-sm font-semibold text-txt transition-colors hover:bg-surface2"
        >
          <EnvelopeIcon className="h-4 w-4" aria-hidden="true" />
          Contact us
        </a>
      </div>
    </section>
  );
}
