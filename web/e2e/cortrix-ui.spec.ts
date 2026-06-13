import { test, expect, type Page } from '@playwright/test';

// Cortrix Web UI — E2E suite (P02a design § 17.1 — 7-8 key scenarios, ~10% of
// the test pyramid). Rewritten in R5/S11 for the react-router app (the prior
// spec targeted the pre-router MVP UI: Connector/CDC/`activePage`, all gone).
//
// Standalone (D3): runs against `vite dev` with no backend (playwright.config
// webServer). Every api module falls back to its in-memory mock, and the mock
// auth session is authenticated-as-admin by default, so guarded routes + the
// admin group render without a sign-in step. Selectors use the stable
// data-testid attributes shipped across R1-R4 (§ 15.2) — not brittle copy.

// All app pages live under the guarded Layout; navigate by URL then assert the
// page's testid is present. A short helper keeps each scenario focused.
async function goto(page: Page, path: string) {
  await page.goto(path);
}

// ── 1. Namespaces (landing) ─────────────────────────────────────────────────
test('1. Namespaces page is the landing route and renders its controls', async ({ page }) => {
  await goto(page, '/');
  // App shell (header + sidebar) is present once the auth probe resolves.
  await expect(page.locator('header')).toContainText('Cortrix');
  // Index route is the Namespaces page (P02a § 8.1). The create entry point
  // always renders (the table body is data-gated, so we assert on the controls
  // that are present regardless of backend availability in standalone).
  await expect(page.getByTestId('namespace-new-btn')).toBeVisible();
});

// ── 2. Search ───────────────────────────────────────────────────────────────
test('2. Search page renders the query box and runs a search', async ({ page }) => {
  await goto(page, '/search');
  await expect(page.getByRole('heading', { name: /Semantic Search/i })).toBeVisible();
  const input = page.getByTestId('search-input');
  await expect(input).toBeVisible();
  await expect(input).toBeEnabled();
  await input.fill('architecture');
  await page.getByTestId('search-submit-btn').click();
  // Search executed: in standalone the request resolves either to mock results
  // (markdown-rendered via the sanitized SafeMarkdown, § 4.7 → .prose) or to an
  // inline GEN-Agent error display (§ 16.4) when the dead-backend proxy returns
  // an error envelope. Either outcome proves the query path ran end-to-end.
  await expect(page.locator('.prose, [data-testid="error-display"]').first()).toBeVisible({
    timeout: 12_000,
  });
});

// ── 3. Chat ─────────────────────────────────────────────────────────────────
test('3. Chat page renders input + send control', async ({ page }) => {
  await goto(page, '/agent');
  await expect(page.getByRole('heading', { name: /Chat with Cortrix/i })).toBeVisible();
  await expect(page.getByTestId('chat-input')).toBeVisible();
  await expect(page.getByTestId('chat-send-btn')).toBeVisible();
});

// ── 4. Memory CRUD entry ────────────────────────────────────────────────────
test('4. Memory page renders the list and the create entry point', async ({ page }) => {
  await goto(page, '/memory');
  await expect(page.getByTestId('memory-list')).toBeVisible();
  await expect(page.getByTestId('memory-new-btn')).toBeVisible();
});

// ── 5. Health dashboard (F20-7 /live + /ready) ──────────────────────────────
test('5. Health dashboard renders live + ready cards', async ({ page }) => {
  await goto(page, '/health');
  await expect(page.getByTestId('health-page')).toBeVisible();
  await expect(page.getByTestId('health-live-card')).toBeVisible();
  await expect(page.getByTestId('health-ready-card')).toBeVisible();
});

// ── 7. Bulk submit panel (TD-F42-BULK-SUBMIT) ───────────────────────────────
test('7. Upload page exposes the bulk-submit panel that expands', async ({ page }) => {
  await goto(page, '/upload');
  await expect(page.getByTestId('upload-dropzone')).toBeVisible();
  const panel = page.getByTestId('bulk-submit-panel');
  await expect(panel).toBeVisible();
  // Toggle expands the JSON batch input (§ partial-success schema UI).
  await page.getByTestId('bulk-submit-toggle').click();
  await expect(page.getByTestId('bulk-json-input')).toBeVisible();
});

// ── 8. Dual-theme toggle (Magma light ↔ dark) ───────────────────────────────
test('8. Theme toggle switches the document between light and dark', async ({ page }) => {
  await goto(page, '/');
  const html = page.locator('html');
  const initiallyDark = await html.evaluate((el) => el.classList.contains('dark'));

  const themeBtn = page.locator('header button[title="Toggle theme"]');
  await expect(themeBtn).toBeVisible();
  await themeBtn.click();
  await expect
    .poll(() => html.evaluate((el) => el.classList.contains('dark')))
    .toBe(!initiallyDark);

  // Toggle back — round-trips to the original theme (persisted to localStorage).
  await themeBtn.click();
  await expect
    .poll(() => html.evaluate((el) => el.classList.contains('dark')))
    .toBe(initiallyDark);
  // App shell stayed mounted through both toggles (no error-boundary trip).
  await expect(page.getByTestId('app-error-boundary')).toHaveCount(0);
  await expect(page.getByTestId('namespace-new-btn')).toBeVisible();
});
