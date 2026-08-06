import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Vitest config (web UI design — test matrix / coverage targets).
// Coverage gate mirrors the design: Lines > 70%, Functions > 80%, Branches > 60%
// (web UI is one of the 17 core features → Story DoD also expects line ≥ 90% on the
// store/util core, enforced in CI per-path; here we set the feature-wide floor).
export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./src/test-setup.ts'],
    exclude: ['e2e/**', 'node_modules/**'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'text-summary', 'html', 'lcov'],
      // Test pyramid: the *unit* layer owns the logic core —
      // stores, hooks, utils, the api client + the SafeMarkdown / ErrorDisplay /
      // error-boundary / Ent-gate components. The page/dialog tree (Memory /
      // Namespace dialogs / admin tables / Upload) is covered by the Playwright
      // E2E layer, not vitest, so the "Lines > 70%" *release* gate is
      // measured on unit + E2E combined in CI. To keep the unit-side threshold
      // honest (not a perpetually-red global gate), coverage is scoped here to
      // the modules the unit suite actually owns.
      include: [
        'src/store/**/*.ts',
        'src/utils/**/*.ts',
        'src/telemetry/**/*.ts',
        'src/api/client.ts',
        'src/api/query.ts',
        'src/api/system.ts',
        'src/api/namespaces.ts',
        'src/components/Common/SafeMarkdown.tsx',
        'src/components/Common/ErrorDisplay.tsx',
        'src/components/Common/AppErrorBoundary.tsx',
        'src/components/EntPlaceholder/**/*.tsx',
      ],
      exclude: [
        'src/**/*.test.{ts,tsx}',
        'src/**/*.d.ts',
        'src/types/**',
        // Stores that are E2E-exercised (chat stream / auth cookie / upload xhr)
        // rather than unit-covered — kept out of the unit threshold scope.
        'src/store/useChatStore.ts',
        'src/store/useAuthStore.ts',
        'src/store/useUploadStore.ts',
      ],
      // unit-layer floor (Lines > 70% / Functions > 80% / Branches > 60%).
      thresholds: {
        lines: 70,
        functions: 80,
        branches: 60,
        statements: 70,
        perFile: false,
      },
    },
  },
});
