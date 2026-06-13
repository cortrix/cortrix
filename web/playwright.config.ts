import { defineConfig, devices } from '@playwright/test';

// Playwright E2E config (P02a design § 17 — test matrix / § 17.2 4-browser CI).
//
// The 4-browser strategy (§ 17.2): PR runs Chromium only; merge adds Firefox;
// nightly + release run all 4 (Chromium / Firefox / WebKit / Edge). CI selects a
// subset via `--project=<name>`; locally `npx playwright test` runs Chromium.
//
// Standalone (D3): `webServer` boots `vite dev` on :5173 so the spec runs with no
// backend — every api module falls back to its in-memory mock (src/api/*.ts), and
// the mock auth session is authenticated-as-admin by default, so guarded routes
// render directly. mcp__claude-in-chrome agent acceptance stays out of CI (§ 17.5).
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
  // § 17.2 browser matrix — selected per trigger with --project.
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    { name: 'firefox', use: { ...devices['Desktop Firefox'] } },
    { name: 'webkit', use: { ...devices['Desktop Safari'] } },
    // Edge = Chromium channel (nightly/release only).
    { name: 'edge', use: { ...devices['Desktop Edge'], channel: 'msedge' } },
  ],
  webServer: {
    command: 'npm run dev -- --port 5173',
    url: 'http://localhost:5173',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
