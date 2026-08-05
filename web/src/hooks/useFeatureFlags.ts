import { create } from 'zustand';
import { getFeatureFlags } from '../api/system';
import type { FeatureDescriptor, SystemEdition } from '../types/api';

// Feature-flag store (web UI design § 5.3). Loads GET /api/v1/system/features
// (credentials:'include' + X-CSRF-Token are applied by the shared api client)
// and exposes edition + per-feature enabled / placeholder probes. Optional
// features are toggled by `isEnabled`; not-yet-available ones report
// `isPlaceholder` with an `availableIn` milestone string.

interface FeatureFlagsState {
  edition: SystemEdition;
  features: Record<string, FeatureDescriptor>;
  loaded: boolean;
  loadFeatures: () => Promise<void>;
  isEnabled: (key: string) => boolean;
  isPlaceholder: (key: string) => boolean;
  availableIn: (key: string) => string | undefined;
}

export const useFeatureFlagsStore = create<FeatureFlagsState>((set, get) => ({
  edition: 'ce',
  features: {},
  loaded: false,

  loadFeatures: async () => {
    try {
      const data = await getFeatureFlags();
      set({
        edition: data.edition,
        features: data.features ?? {},
        loaded: true,
      });
    } catch {
      // Standalone / offline: stay on the CE default with no enabled features.
      set({ edition: 'ce', features: {}, loaded: true });
    }
  },

  isEnabled: (key) => get().features[key]?.enabled === true,
  isPlaceholder: (key) => get().features[key]?.placeholder === true,
  availableIn: (key) => get().features[key]?.available_in,
}));

// Convenience hook: flat reads for components.
export function useFeatureFlags() {
  const edition = useFeatureFlagsStore((s) => s.edition);
  const isEnabled = useFeatureFlagsStore((s) => s.isEnabled);
  const isPlaceholder = useFeatureFlagsStore((s) => s.isPlaceholder);
  return { edition, isEnabled, isPlaceholder };
}
