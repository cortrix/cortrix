import { get } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type { SystemStatus } from '../types/api';
import type { SystemFeaturesResponse } from '../types/api';

export async function getSystemStatus(): Promise<SystemStatus> {
  return get<SystemStatus>('/api/v1/system/status');
}

export async function getFeatureFlags(): Promise<SystemFeaturesResponse> {
  // Mock fallback is build-time gated (./fallback.ts): production surfaces the
  // error (the caller — useFeatureFlagsStore — then applies its own CE default);
  // only a standalone build falls back to the CE mock here.
  try {
    return await get<SystemFeaturesResponse>('/api/v1/system/features');
  } catch (e) {
    return fallbackToMock(e, () => mockApi.getFeatureFlags());
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
