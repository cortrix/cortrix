# Cortrix Web UI - E2E Test Cases

**Project**: Cortrix Semantic Storage Engine - Web UI
**Version**: v0.1.0-mvp
**Test Type**: End-to-End Browser Automation
**Test Date**: 2026-02-18
**Tester**: QA Test Expert (Automated Playwright)
**Environment**: macOS, Chromium headless, 1440x900 viewport

---

## Test Strategy Overview

### Scope and Objectives

Test all six features of the Cortrix Web UI running at `http://localhost:5173`, verifying:
- Complete user flows from UI interaction to backend API response
- Data correctness (files upload and reach Completed status)
- Error handling and status feedback
- Navigation and component rendering

### Services Under Test

| Service | URL | Role |
|---------|-----|------|
| Vite Dev Server (React UI) | `http://localhost:5173` | Frontend SPA |
| C++ Cortrix Engine | `http://localhost:8420` | Storage/Search API (`/api` proxy) |
| Cortrix Agent (FastAPI) | `http://localhost:8001` | Chat/LLM API (`/agent` proxy) |

### Vite Proxy Configuration

```
/api  → http://localhost:8420    (C++ backend)
/agent → http://localhost:8001   (FastAPI, strips /agent prefix)
```

### Risk Areas (Prioritized)

1. **HIGH**: Search results rendering - `block_id` type mismatch causes React crash (CONFIRMED BUG)
2. **HIGH**: Upload file pipeline - most user-facing critical path
3. **MEDIUM**: Chat LLM response latency - depends on external GLM API
4. **LOW**: Connector scan feature - backend integration only, no external deps

---

## Test Environment Requirements

### Prerequisites

- Node.js + pnpm installed, `cd web && pnpm dev` running on port 5173
- C++ Cortrix binary running: `./cortrix-server --config config.yaml` on port 8420
- Cortrix Agent running: `cd cortrix-agent && uvicorn main:app --port 8001`
- Test files present:
  - `/tmp/test_cortrix_upload.pdf` (1.1 KB test PDF)
  - `/Users/derek/Documents/test-cortrix-docs/cortrix_intro.md` (2.0 KB Markdown)
- Python with `playwright` installed: `pip install playwright && playwright install chromium`

### Test Data Requirements

- Namespace "default" exists (created automatically on first start)
- Namespace "demo" exists (pre-created for testing)
- GLM API key configured in cortrix-agent (for Chat tests)

---

## Smoke Test Suite (Run Before Any Release)

These 8 tests must pass before proceeding with any release:

| # | Test Case | Feature | Expected |
|---|-----------|---------|---------|
| 1 | TC-U01 | Upload | Upload page loads with drop zone |
| 2 | TC-U05 | Upload | PDF upload reaches "Completed" status |
| 3 | TC-S01 | Search | Search page loads with input |
| 4 | TC-S05 | Search | Search API call returns HTTP 200 |
| 5 | TC-C01 | Chat | Chat page loads with input |
| 6 | TC-C08 | Chat | Message can be sent via Enter key |
| 7 | TC-N01 | Namespace | Namespace selector shows in header |
| 8 | TC-K01 | Connector | Directory Monitor page loads |

---

## Test Suite 1: Upload Feature (MOST CRITICAL)

The upload feature is the primary data ingestion path. All tests must pass.

### TC-U01: Navigate to Upload Page

**Priority**: Critical
**Category**: Navigation

**Preconditions**: App running at http://localhost:5173

**Test Steps**:
1. Open browser to http://localhost:5173
2. Wait 1.5 seconds for app to initialize
3. Click "Upload" button in sidebar

**Expected Results**:
- Page title "Upload Documents" visible as `<h1>`
- Subtitle "Upload files to process through the Semantic Processing Pipeline" visible
- Upload drop zone rendered

**Test Result**: PASS

---

### TC-U02: Drop Zone UI Elements

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Navigate to Upload page (TC-U01)
2. Verify text content of drop zone area

**Expected Results**:
- "Drag & drop files here" text visible in drop zone center
- "or click to browse" hint visible below
- "Supports PDF, Word, Markdown, TXT, Images (max 100MB)" format hint visible
- Drop zone has dashed border styling

**Test Result**: PASS

---

### TC-U03: File Input Element Present

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Navigate to Upload page
2. Inspect DOM for `input[type='file']` element

**Expected Results**:
- Hidden `input[type='file']` element exists in DOM
- Element accepts: application/pdf, text/plain, text/markdown, .docx, image/png, image/jpeg
- Max file size: 100MB

**Test Result**: PASS

---

### TC-U04: Supported Formats Hint

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- "Supports PDF, Word, Markdown, TXT, Images (max 100MB)" text visible

**Test Result**: PASS

---

### TC-U05: PDF File Upload - Full Flow

**Priority**: Critical
**Category**: E2E

**Test Steps**:
1. Navigate to Upload page
2. Set file `/tmp/test_cortrix_upload.pdf` on the hidden file input
3. Wait up to 20 seconds for status change
4. Verify status badge

**Expected Results**:
- File appears in UPLOADS list immediately after selection
- Status transitions: pending → uploading (progress bar) → processing → Completed
- File shows "Completed" green badge (from i18n: `upload.statusCompleted`)
- File name "test_cortrix_upload.pdf" and size "1.1 KB" visible in list
- Backend API POST `/api/v1/namespaces/default/documents` returns HTTP 201
- Document status GET `/api/v1/namespaces/default/documents/{id}/status` returns `"status": "ready"`

**Edge Cases**:
- Duplicate file upload: should succeed (same content_hash, new doc_id)
- File > 100MB: should show error
- Unsupported file type (.exe): should be rejected by dropzone before upload

**Potential Failure Points**:
- Backend ONNX embedding model not loaded → status stays "processing"
- Network timeout on large files → status shows "error"

**Test Result**: PASS

---

### TC-U06: Markdown File Upload - Full Flow

**Priority**: Critical
**Category**: E2E

**Test Steps**:
1. Navigate to Upload page
2. Set file `/Users/derek/Documents/test-cortrix-docs/cortrix_intro.md` on file input
3. Wait up to 20 seconds for Completed status

**Expected Results**:
- "cortrix_intro.md" with "2.0 KB" visible in UPLOADS list
- Status badge shows "Completed"
- UPLOADS section shows count "(2)" after both files uploaded
- Both PDF and Markdown appear in list simultaneously

**Test Result**: PASS

---

### TC-U07: UPLOADS Count Heading

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- `<h2>` heading with text "UPLOADS (2)" visible (or appropriate count)
- Count updates after each file upload

**Test Result**: PASS (heading shows "UPLOADS (2)")

---

### TC-U08: No HTTP Errors During Upload

**Priority**: Critical
**Category**: Integration

**Expected Results**:
- POST `/api/v1/namespaces/{ns}/documents` returns HTTP 201
- GET `/api/v1/namespaces/{ns}/documents/{id}/status` returns HTTP 200
- No 4xx or 5xx responses from any upload-related endpoint

**Test Result**: PASS

---

### TC-U09: No JS Console Errors During Upload

**Priority**: High
**Category**: Error Handling

**Expected Results**:
- Browser console shows no `[error]` level messages during entire upload flow
- No unhandled promise rejections

**Test Result**: PASS

---

## Test Suite 2: Search Feature

**KNOWN BUG**: TC-S06 currently FAILS due to a type mismatch bug. See Bug Report section.

### TC-S01: Navigate to Search Page

**Priority**: Critical
**Category**: Navigation

**Test Steps**:
1. Click "Search" in sidebar

**Expected Results**:
- Page title "Semantic Search" visible
- Subtitle "Search across all documents using natural language" visible

**Test Result**: PASS

---

### TC-S02: Search Input Element

**Priority**: High
**Category**: Functional

**Expected Results**:
- `<input type="text">` with placeholder "Search across all documents using natural language..." visible
- Input accepts text entry
- Input has search icon on left

**Test Result**: PASS

---

### TC-S03: Search Submit Button

**Priority**: High
**Category**: Functional

**Expected Results**:
- `<button type="submit">` with text "Search" visible in main content area (not sidebar)
- Button is disabled when input is empty (`disabled` attribute present when `!query.trim()`)
- Button enables after text is entered

**Test Result**: PASS

---

### TC-S04: Filter Dropdown Controls

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- "All Types" `<select>` dropdown with options: All Types, FILE, DATABASE, MEMORY
- "Top 10" `<select>` dropdown with options: Top 10, Top 20, Top 50

**Test Result**: PASS

---

### TC-S05: Search API Call Succeeds

**Priority**: Critical
**Category**: Integration

**Test Steps**:
1. Navigate to Search page
2. Fill input with "Cortrix embedding model"
3. Click submit button

**Expected Results**:
- POST `http://localhost:5173/api/v1/query` (proxied to 8420) returns HTTP 200
- Response JSON contains `results` array and `meta` object
- `meta.routes_used` includes "vector" and "bm25"
- `meta.latency_ms` is populated
- `results` array has 10 items (Top 10 default)

**Test Result**: PASS (HTTP 200, results returned by backend)

---

### TC-S06: Search Results Render in UI [FAILING - BUG]

**Priority**: Critical
**Category**: E2E

**Test Steps**:
1. Submit search query "Cortrix embedding model"
2. Wait for API response
3. Verify result cards rendered in page

**Expected Results**:
- Result cards rendered showing:
  - Block type badge (FILE/DATABASE/MEMORY)
  - Source path (e.g., `/Users/derek/Documents/test-cortrix-docs/cortrix_intro.md`)
  - Relevance score (e.g., `0.016`)
  - Chunk text content (rendered as Markdown via react-markdown)
  - Block ID, Document ID, related block count in footer
- Summary line shows result count and routes used (vector + bm25 RRF)

**Actual Result**: BLANK WHITE PAGE after API returns results

**Root Cause**: See Bug Report BUG-001

**Test Result**: FAIL (confirmed bug)

---

### TC-S07: No HTTP Errors from Search API

**Priority**: High
**Category**: Integration

**Expected Results**:
- All search-related API calls return HTTP 2xx

**Test Result**: PASS

---

## Test Suite 3: Chat Feature

### TC-C01: Navigate to Chat Page

**Priority**: Critical
**Category**: Navigation

**Test Steps**:
1. Click "Chat" in sidebar (navigates via `setActivePage('chat')`)

**Expected Results**:
- `<h1>` heading "Chat with Cortrix" visible
- Subtitle "Semantic Search + Memory + RAG" visible

**Test Result**: PASS

---

### TC-C02: RAG Mode Subtitle

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- Subtitle "Semantic Search + Memory + RAG" confirms full RAG pipeline active

**Test Result**: PASS

---

### TC-C03: Chat Input Element

**Priority**: Critical
**Category**: Functional

**Expected Results**:
- `<input>` element (NOT textarea) with placeholder "Ask anything about your documents..." visible
- Input positioned at bottom of page in fixed footer area

**Test Result**: PASS

---

### TC-C04: Send Button

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Send" button with arrow/send icon visible next to chat input
- Button is enabled when input has text

**Test Result**: PASS

---

### TC-C05: History Button with Count

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- "History (N)" button in top-right of chat page
- N shows actual session count from `/agent/sessions` API
- Count increments as new sessions are created

**Test Result**: PASS (showed "History (16)")

---

### TC-C06: New Session Button

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- "New Session" button in top-right next to History
- Creates a fresh conversation session when clicked

**Test Result**: PASS

---

### TC-C07: Enter-to-Send Hint

**Priority**: Low
**Category**: Usability

**Expected Results**:
- "Enter to send · Powered by RAG + Semantic Search + Memory" hint text visible below input

**Test Result**: PASS

---

### TC-C08: Send Message via Enter Key

**Priority**: Critical
**Category**: Functional

**Test Steps**:
1. Navigate to Chat page
2. Click chat input field
3. Type "What is Cortrix's core architecture?"
4. Press Enter

**Expected Results**:
- Message appears as user bubble (right-aligned, blue background)
- Input clears after sending
- "Thinking..." loading indicator appears for assistant response

**Test Result**: PASS

---

### TC-C09: LLM Generates Response

**Priority**: Critical
**Category**: E2E

**Test Steps**:
1. Send message "What is Cortrix's core architecture?"
2. Wait up to 45 seconds for response

**Expected Results**:
- "Thinking..." indicator disappears
- Assistant response appears (left-aligned)
- Response contains architecture-related content (storage, hybrid retrieval, vector, etc.)
- Response is grounded in indexed documents (RAG - Retrieval Augmented Generation)

**Note**: The LLM response uses streaming (SSE). Playwright may log `requestfailed` warnings for streaming responses - this is expected browser behavior with SSE, not an actual failure. The cortrix-agent session shows `interaction_count: 2` confirming successful exchanges.

**Test Result**: PASS (response received with architecture content)

---

### TC-C10: Follow-up Message (Context Continuity)

**Priority**: High
**Category**: E2E

**Test Steps**:
1. After first response, type "What file formats does it support?"
2. Press Enter

**Expected Results**:
- Follow-up message sent and visible
- Response references file format context (PDF, Word, Markdown, TXT, Images)
- Conversation history maintained within session (Memory feature active)

**Test Result**: PASS

---

### TC-C11: History Dropdown Shows Sessions

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Click "History (N)" button

**Expected Results**:
- Dropdown opens showing list of past sessions
- Each session shows title ("New Chat") and date (e.g., "2/18/2026")
- Sessions have message count (e.g., "8 messages")
- Active session highlighted

**Test Result**: PASS (sessions visible with "New Chat" entries and message counts)

---

### TC-C12: No HTTP Errors from Chat API

**Priority**: High
**Category**: Integration

**Expected Results**:
- `/agent/sessions` GET returns HTTP 200
- `/agent/sessions` POST (create session) returns HTTP 200/201
- `/agent/chat` POST streaming returns HTTP 200

**Test Result**: PASS

---

## Test Suite 4: Namespace Management

### TC-N01: Namespace Selector in Header

**Priority**: High
**Category**: Functional

**Expected Results**:
- Namespace dropdown button visible in header showing "default" with chevron icon

**Test Result**: PASS

---

### TC-N02: Namespace Dropdown Opens

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Click "default" button in header

**Expected Results**:
- Dropdown popover opens
- Shows namespace list loaded from `/api/v1/namespaces`

**Test Result**: PASS

---

### TC-N03: "default" Namespace Entry

**Priority**: High
**Category**: Functional

**Expected Results**:
- "default" entry with "0 docs" count in dropdown

**Test Result**: PASS

---

### TC-N04: "demo" Namespace from API

**Priority**: High
**Category**: Integration

**Expected Results**:
- "demo" namespace visible, confirming API loaded namespace list
- Backend `GET /api/v1/namespaces` returned both "default" and "demo"

**Test Result**: PASS

---

### TC-N05: Doc Count Per Namespace

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- Each namespace entry shows "N docs" label
- Count reflects actual document count from API

**Test Result**: PASS

---

### TC-N06: New Namespace Creation Option

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- "+ New namespace" option visible at bottom of dropdown
- Clicking should open namespace creation dialog (not tested in this suite)

**Test Result**: PASS

---

### TC-N07: Dropdown Dismisses on Escape

**Priority**: Low
**Category**: Usability

**Expected Results**:
- Pressing Escape key closes namespace dropdown
- Background page interaction not blocked after dismissal

**Test Result**: PASS

---

## Test Suite 5: LLM Settings

### TC-L01: LLM Badge in Header

**Priority**: High
**Category**: Functional

**Expected Results**:
- Green dot + "LLM: GLM-4" badge visible in header right area
- Badge indicates LLM is connected and configured

**Test Result**: PASS

---

### TC-L02: Click Opens LLM Configuration Dialog

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Click "LLM: GLM-4" badge in header

**Expected Results**:
- Modal dialog "LLM Configuration" opens
- Background overlaid with semi-transparent black mask

**Test Result**: PASS

---

### TC-L03: Dialog Title

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- Dialog shows title "LLM Configuration"

**Test Result**: PASS

---

### TC-L04: Provider Dropdown

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Provider" label visible
- `<select>` dropdown present
- "GLM (Zhipu AI)" option available and currently selected

**Test Result**: PASS

---

### TC-L05: Model Dropdown

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Model" label visible
- `<select>` dropdown present
- "GLM-4 Flash — Free tier, fast" option available
- "GLM-4 FlashX — Free tier, extra fast" also available as option

**Test Result**: PASS

---

### TC-L06: API Key Field

**Priority**: High
**Category**: Functional

**Expected Results**:
- "API Key" label with "leave blank to keep current" hint visible
- `<input type="password">` (with toggle eye icon) with placeholder "Enter API Key..."
- Current key masked, user can update by entering new key

**Test Result**: PASS

---

### TC-L07: Cancel and Save Buttons

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Cancel" button (gray) and "Save" button (blue) visible in dialog footer

**Test Result**: PASS

---

### TC-L08: Cancel Closes Dialog

**Priority**: High
**Category**: Functional

**Test Steps**:
1. Click Cancel button

**Expected Results**:
- Dialog dismisses immediately
- No changes saved
- Background page re-enabled for interaction

**Test Result**: PASS

---

## Test Suite 6: Connector / Directory Monitor

### TC-K01: Navigate to Connector Page

**Priority**: High
**Category**: Navigation

**Expected Results**:
- Page title "Directory Monitor" visible
- Subtitle "Monitor local directories for automatic document ingestion"

**Test Result**: PASS

---

### TC-K02: Subtitle Visible

**Priority**: Low
**Category**: Functional

**Test Result**: PASS

---

### TC-K03: Configure Watch Directory Section

**Priority**: High
**Category**: Functional

**Expected Results**:
- Section header "Configure Watch Directory" with icon visible
- Section subtitle "Set the local directory to monitor for new documents"

**Test Result**: PASS

---

### TC-K04: Directory Path Shows Configured Value

**Priority**: High
**Category**: Integration

**Expected Results**:
- "Directory Path" label visible
- Text input shows `/Users/derek/Documents/test-docs` (loaded from API)
- Value loaded from `GET /api/v1/connector/status` response field `watch_dir`

**Test Result**: PASS

---

### TC-K05: Target Namespace Shows "local"

**Priority**: High
**Category**: Integration

**Expected Results**:
- "Target Namespace" label visible
- Input shows "local" (loaded from API `namespace_name` field)

**Test Result**: PASS

---

### TC-K06: Apply Button

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Apply" button with icon visible
- Clicking sends `POST /api/v1/connector/watch` with updated directory path

**Test Result**: PASS

---

### TC-K07: Watched Directory Status Section

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Watched Directory" section with current path "/Users/derek/Documents/test-docs"

**Test Result**: PASS

---

### TC-K08: Status Badge Shows "Stopped"

**Priority**: High
**Category**: Integration

**Expected Results**:
- Status badge shows "Stopped" (when `watching: false` from API)
- Badge color: gray/amber for stopped state
- If watching were active, badge would show "Watching" in green

**Test Result**: PASS

---

### TC-K09: Scan Now Button

**Priority**: High
**Category**: Functional

**Expected Results**:
- "Scan Now" button with play icon visible
- Clicking triggers `POST /api/v1/connector/scan` to manually scan directory

**Test Result**: PASS

---

### TC-K10: Statistics Cards

**Priority**: Medium
**Category**: Functional

**Expected Results**:
- Four stat cards visible: "Total Files", "Imported", "Updated", "Skipped"
- Each shows a number (0 in fresh state)
- Numbers loaded from `GET /api/v1/connector/stats`

**Test Result**: PASS

---

### TC-K11: Backend Connector API Returns Correct Status

**Priority**: High
**Category**: Integration

**API Call**: `GET http://localhost:8420/api/v1/connector/status`

**Expected Response**:
```json
{
  "enabled": true,
  "watching": false,
  "watch_dir": "/Users/derek/Documents/test-docs",
  "namespace_name": "local",
  "status": "stopped"
}
```

**Test Result**: PASS

---

## Bug Reports

### BUG-001: Search Results Page Renders Blank (HIGH severity)

**Status**: CONFIRMED BUG
**Severity**: HIGH
**Feature**: Search
**Test Case**: TC-S06

**Description**:
After a successful search API call returning 10 results (HTTP 200), the search results area renders as a blank white page. No result cards are displayed.

**Root Cause Analysis**:

In `web/src/components/Search/ResultItem.tsx`:
```tsx
<span>{t('search.block', { id: result.block_id.replace('blk-', '') })}</span>
```

The TypeScript type `SearchResult.block_id` is declared as `string` in `web/src/types/api.ts`:
```typescript
export interface SearchResult {
  block_id: string;   // declared as string
  ...
}
```

However, the C++ backend (`/api/v1/query`) returns `block_id` as a **JSON integer**:
```json
{"block_id": 23, "block_type": "FILE", ...}
```

At runtime, `result.block_id` is an `integer` (e.g., `23`). Calling `.replace()` on a number throws:
```
TypeError: result.block_id.replace is not a function
```

This React render error propagates up and causes the entire `SearchResults` component to render as blank (React error boundary swallows the error without showing a visible error message in production-mode builds).

**Fix Options**:

Option A (Frontend - recommended, minimal change):
```tsx
// In web/src/components/Search/ResultItem.tsx
// Replace:
{ id: result.block_id.replace('blk-', '') }
// With:
{ id: String(result.block_id).replace('blk-', '') }
```

Option B (Backend):
Change the C++ response serialization to format `block_id` as a string (e.g., `"blk-23"`).

Option C (Type definition):
Change type to `block_id: number | string` and cast in ResultItem.

**Recommendation**: Apply Option A immediately - one-line fix, zero risk, unblocks search feature.

**Reproduction Steps**:
1. Navigate to http://localhost:5173
2. Click "Search" in sidebar
3. Type any query (e.g., "Cortrix")
4. Click Search button
5. Observe: page goes blank instead of showing results

---

## Test Execution Results Summary

**Execution Date**: 2026-02-18
**Test Script**: `/tmp/cortrix-e2e/test_cortrix_definitive.py`
**Screenshots**: `/tmp/cortrix-e2e/def_*.png`

| Feature | Tests | Passed | Failed | Status |
|---------|-------|--------|--------|--------|
| Upload | 9 | 9 | 0 | PASS |
| Search | 7 | 6 | 1 | FAIL (BUG-001) |
| Chat | 12 | 12 | 0 | PASS |
| Namespace | 7 | 7 | 0 | PASS |
| LLM Settings | 8 | 8 | 0 | PASS |
| Connector | 11 | 11 | 0 | PASS |
| **TOTAL** | **54** | **53** | **1** | **1 BUG** |

---

## Acceptance Criteria Checklist

### Upload Feature
- [x] PDF files upload successfully and reach "Completed" status
- [x] Markdown files upload successfully and reach "Completed" status
- [x] Progress feedback shown during upload
- [x] File list shows uploaded files with name, size, and status badge
- [x] No HTTP errors during upload flow
- [x] No JavaScript console errors

### Search Feature
- [x] Search input renders with correct placeholder
- [x] Filter dropdowns (All Types, Top N) visible and functional
- [x] Search API call returns HTTP 200 with results
- [ ] **BLOCKED**: Result cards DO NOT render (BUG-001 - block_id type mismatch)

### Chat Feature
- [x] Chat page loads with correct UI elements
- [x] Message input (input element) works with Enter key to send
- [x] LLM (GLM-4 Flash via cortrix-agent) generates responses
- [x] Follow-up messages maintain conversation context
- [x] History (N) dropdown shows session list
- [x] New Session button works

### Namespace Management
- [x] Namespace selector shows current namespace in header
- [x] Dropdown loads namespace list from API
- [x] "default" and "demo" namespaces visible
- [x] "+ New namespace" creation option available

### LLM Settings
- [x] LLM badge visible in header with provider/model info
- [x] Click opens LLM Configuration dialog
- [x] Provider, Model, API Key fields all present
- [x] Cancel closes dialog without changes

### Connector / Directory Monitor
- [x] Directory Monitor page loads correctly
- [x] Watch directory path loaded from API
- [x] Target namespace loaded from API
- [x] Status badge shows correct state (Stopped/Watching)
- [x] Scan Now and Apply buttons present
- [x] Statistics cards visible

---

## Production Readiness Assessment

**Overall Status**: NOT READY - 1 HIGH severity bug blocking Search feature

### Required Before Release
- [ ] **BUG-001 MUST be fixed**: Search results blank page (`block_id` type mismatch in `ResultItem.tsx`)
  - Fix is trivial: change `result.block_id.replace(...)` to `String(result.block_id).replace(...)`
  - After fix, re-run TC-S06 to verify result cards render correctly

### Recommended Before Release
- [ ] Add error boundary around `SearchResults` with user-visible error message instead of blank page
- [ ] Test upload with files > 10MB to verify progress bar works
- [ ] Test search with empty namespace (no documents) to verify empty state message
- [ ] Test LLM Settings "Save" button with valid API key change

### Post-Release Monitoring
- Chat response time (LLM latency via external GLM API)
- Upload processing time (ONNX embedding inference)
- Connector scan interval and file detection reliability

---

## Automation Reference

**Test Script Location**: `/tmp/cortrix-e2e/test_cortrix_definitive.py`

**Run Command**:
```bash
cd /tmp/cortrix-e2e
python3 test_cortrix_definitive.py
```

**Screenshot Directory**: `/tmp/cortrix-e2e/def_*.png`

**Key Screenshots**:
- `def_05_md_result.png` - Both files showing "Completed" status
- `def_23_chat_response.png` - Chat "Thinking..." state (LLM responding)
- `def_41_llm_dialog.png` - LLM Configuration dialog
- `def_50_connector_page.png` - Directory Monitor full page
- `def_31_ns_dropdown.png` - Namespace dropdown open
