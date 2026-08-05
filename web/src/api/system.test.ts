import { describe, it, expect } from 'vitest';
import { parseLlmStatus } from './system';
import type { SystemStatus } from '../types/api';

// parseLlmStatus (web UI — Header LLM badge derivation). Pure mapping from the
// system status payload to the UI's llmProvider / llmEnabled shape.

describe('parseLlmStatus', () => {
  it('reflects enabled provider', () => {
    const s = { llm_provider: 'openai', llm_enabled: true } as SystemStatus;
    expect(parseLlmStatus(s)).toEqual({ llmProvider: 'openai', llmEnabled: true });
  });

  it('defaults to disabled / null when fields are absent', () => {
    const s = {} as SystemStatus;
    expect(parseLlmStatus(s)).toEqual({ llmProvider: null, llmEnabled: false });
  });
});
