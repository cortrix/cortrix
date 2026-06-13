import { get } from './client';
import { mockApi } from './mock';
import type { SystemStatus } from '../types/api';
import type { SystemFeaturesResponse } from '../types/api';

export async function getSystemStatus(): Promise<SystemStatus> {
  return get<SystemStatus>('/api/v1/system/status');
}

export async function getFeatureFlags(): Promise<SystemFeaturesResponse> {
  // Standalone (D3): fall back to the CE mock when the backend is unreachable.
  try {
    return await get<SystemFeaturesResponse>('/api/v1/system/features');
  } catch {
    return mockApi.getFeatureFlags();
  }
}

export function parseLlmStatus(status: SystemStatus): {
  llmProvider: string | null;
  llmEnabled: boolean;
} {
  return {
    llmProvider: status.llm_provider ?? null,
    llmEnabled: status.llm_enabled ?? false,
  };
}
