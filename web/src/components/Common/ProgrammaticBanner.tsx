import { CommandLineIcon } from '@heroicons/react/24/outline';
import type { ReactNode } from 'react';

// Agent-friendly "Programmatic equivalent" banner (web UI design). Every
// screen that exposes a CRUD action also surfaces the SDK / MCP / REST
// equivalent so an Agent can discover the programmatic path. Magma VI: codebg
// surface + mono snippet (SoT: design/vi-mockup/cortrix-vi-console.html
// "Prefer to drive this with an Agent?" panel).

interface ProgrammaticBannerProps {
  /** Inline code snippet, e.g. `kit.cortrix_memory_list(user_id="…")`. */
  snippet: ReactNode;
  /** Optional comment line shown above the snippet. */
  comment?: string;
  /** Channels shown on the right, defaults to "SDK · MCP · REST". */
  channels?: string;
  className?: string;
}

export function ProgrammaticBanner({
  snippet,
  comment = 'Every action here has a programmatic equivalent',
  channels = 'SDK · MCP · REST',
  className = '',
}: ProgrammaticBannerProps) {
  return (
    <section
      aria-label="Programmatic equivalent"
      data-testid="programmatic-banner"
      className={`overflow-hidden rounded-xl border border-line bg-codebg card-shadow ${className}`}
    >
      <div className="flex items-center justify-between border-b border-line px-4 py-2.5">
        <div className="flex items-center gap-2 text-xs text-muted">
          <CommandLineIcon className="h-4 w-4 text-magma-h" aria-hidden="true" />
          Prefer to drive this with an Agent?
        </div>
        <span className="font-mono text-[11px] text-muted">{channels}</span>
      </div>
      <div className="px-4 py-3 font-mono text-[13px] leading-relaxed">
        <div className="text-muted"># {comment}</div>
        <div className="mt-1 text-txt">{snippet}</div>
      </div>
    </section>
  );
}
