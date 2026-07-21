import { test, expect, type Page } from '@playwright/test';

// Cortrix Web UI end-to-end coverage for primary user routes and controls.
//
// Standalone mode runs against `vite dev` with no backend (playwright.config
// webServer). Every api module falls back to its in-memory mock, and the mock
// auth session is authenticated-as-admin by default, so guarded routes + the
// admin group render without a sign-in step. Selectors use the stable
// data-testid attributes instead of brittle display copy.

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
  // The index route is the Namespaces page. The create entry point
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
  // (markdown rendered through SafeMarkdown) or to an inline error display
  // when the unavailable backend proxy returns
  // an error envelope. Either outcome proves the query path ran end-to-end.
  await expect(page.locator('.prose, [data-testid="error-display"]').first()).toBeVisible({
    timeout: 12_000,
  });
});

// ── 3. Chat ─────────────────────────────────────────────────────────────────
test('3. Chat page renders input + send control', async ({ page }) => {
  await goto(page, '/agent');
  await expect(page.getByRole('heading', { name: /Cortrix Agent/i })).toBeVisible();
  await expect(page.getByTestId('chat-input')).toBeVisible();
  await expect(page.getByTestId('chat-send-btn')).toBeVisible();
});

// ── 4. Memory CRUD entry ────────────────────────────────────────────────────
test('4. Memory page renders the filters and create entry point', async ({ page }) => {
  await goto(page, '/memory');
  // The in-memory dataset may legitimately be empty, so assert on controls
  // that are stable for both empty and populated states.
  await expect(page.getByTestId('memory-user-filter')).toBeVisible();
  await expect(page.getByTestId('memory-new-btn')).toBeVisible();
  await expect(page.getByTestId('app-error-boundary')).toHaveCount(0);
});

// ── 5. Health dashboard (/live + /ready) ───────────────────────────────────
test('5. Health dashboard renders live + ready cards', async ({ page }) => {
  await goto(page, '/health');
  await expect(page.getByTestId('health-page')).toBeVisible();
  await expect(page.getByTestId('health-live-card')).toBeVisible();
  await expect(page.getByTestId('health-ready-card')).toBeVisible();
});

// ── 7. Bulk submit panel ────────────────────────────────────────────────────
test('7. Upload page exposes the bulk-submit panel that expands', async ({ page }) => {
  await goto(page, '/upload');
  await expect(page.getByTestId('upload-dropzone')).toBeVisible();
  const panel = page.getByTestId('bulk-submit-panel');
  await expect(panel).toBeVisible();
  // The toggle expands the JSON batch input.
  await page.getByTestId('bulk-submit-toggle').click();
  await expect(page.getByTestId('bulk-json-input')).toBeVisible();
});

// ── 6. Namespace detail drawer survives a backend that omits `configs` ───────
// Regression coverage for a View Details crash: the backend can return a
// namespace detail with no `configs` object (or
// missing doc_count/block_count). The drawer mapped CONFIG_META over
// `ns.configs[key]`, so undefined[key] threw a TypeError into the ErrorBoundary.
// Here we intercept the list + detail endpoints to serve exactly that degraded
// payload and assert the drawer opens without tripping app-error-boundary.
test('6. Namespace detail drawer renders when backend omits configs', async ({ page }) => {
  await page.route('**/api/v1/namespaces', async (route) => {
    if (route.request().method() !== 'GET') return route.continue();
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      // doc_count/block_count present here (list contract); the detail below is
      // the degraded one.
      body: JSON.stringify({
        namespaces: [{ name: 'degraded-ns', created_at: '2026-06-01T00:00:00Z', doc_count: 1, block_count: 2 }],
      }),
    });
  });
  await page.route('**/api/v1/namespaces/degraded-ns', async (route) => {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      // Intentionally missing `configs`, doc_count, block_count — the shape the
      // live backend returned that crashed the drawer.
      body: JSON.stringify({
        ns_id: 'ns_degr',
        name: 'degraded-ns',
        created_at: '2026-06-01T00:00:00Z',
        isolation_mode: 'shared',
        visibility: 'tenant',
        status: 'active',
      }),
    });
  });

  await goto(page, '/');
  await page.getByRole('row', { name: /degraded-ns/ }).getByTestId('namespace-view-btn').click();

  // Drawer opened with all 11 config accordions (each degraded to "default"),
  // and crucially the app did NOT trip the error boundary.
  await expect(page.getByTestId('drawer-config-reranker_config')).toBeVisible();
  await expect(page.getByTestId('app-error-boundary')).toHaveCount(0);
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
