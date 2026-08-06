import { defineConfig, devices } from '@playwright/test';

// Playwright E2E config (web UI design — test matrix / 4-browser CI).
//
// The 4-browser strategy: PR runs Chromium only; merge adds Firefox;
// nightly + release run all 4 (Chromium / Firefox / WebKit / Edge). CI selects a
// subset via `--project=<name>`; locally `npx playwright test` runs Chromium.
//
// Standalone: `webServer` boots `vite dev` on :5173 so the spec runs with no
// backend. The mock fallback is build-time gated (src/api/fallback.ts) — OFF in a
// production build — so the webServer environment sets VITE_USE_MOCK=1 to force it on,
// letting every api module fall back to its in-memory mock and the mock
// authenticated-as-admin session render the guarded routes directly.
// Interactive browser acceptance is verified separately from automated CI.
export default defineConfig({
  testDir: './e2e',
  timeout: 30_000,
  expect: { timeout: 8_000 },
  fullyParallel: true,
  retries: process.env.CI ? 1 : 0,
  reporter: 'list',
  use: {
    baseURL: 'http://localhost:5173',
    headless: true,
    screenshot: 'only-on-failure',
    trace: 'on-first-retry',
  },
  // browser matrix — selected per trigger with --project.
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] } },
    { name: 'webkit', use: { ...devices['Desktop Safari'] } },
    // Edge = Chromium channel (nightly/release only).
    { name: 'edge', use: { ...devices['Desktop Edge'], channel: 'msedge' } },
  ],
  webServer: {
    command: 'npm run dev -- --port 5173',
    env: { VITE_USE_MOCK: '1' },
    url: 'http://localhost:5173',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
