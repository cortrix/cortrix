import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Vite build + dev config (P02a design § 11.3 proxy + § 14.2 code-splitting).
//
// Build output is emitted to static/ (committed, embedded into cortrix-server
// at container build per § 11.1 / F24). manualChunks splits the vendor graph so
// the initial bundle stays under the 250KB gzip release gate (§ 14.1): the big
// libraries (react / state / ui / monaco) land in long-cached vendor chunks and
// Monaco + the lazy route pages (see src/routes.tsx) are only fetched on demand.
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: 'static',
    emptyOutDir: true,
    // Keep the warning honest now that the largest deps are split out.
    chunkSizeWarningLimit: 600,
    rollupOptions: {
      output: {
        // § 14.2 vendor chunking. Monaco (editor-vendor) is the heaviest and is
        // reached only via the React.lazy JsonEditor, so it is pulled fully out
        // of the entry graph here + on demand at the import site.
        manualChunks: {
          'react-vendor': ['react', 'react-dom', 'react-router-dom'],
          'state-vendor': ['zustand', '@tanstack/react-query'],
          'ui-vendor': ['@headlessui/react', '@heroicons/react'],
          'editor-vendor': ['@monaco-editor/react'],
        },
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      // § 11.3 — API + agent + OTLP metrics all proxied to the local backend in
      // dev so the SPA runs same-origin (matches the production reverse proxy;
      // no CORS, CSP connect-src stays 'self'). Backend API port = 8080 (F24
      // real contract). OTLP metrics (§ 23-bis.2) post to /v1/metrics.
      '/api': 'http://localhost:8080',
      '/v1/metrics': 'http://localhost:8080',
      '/agent': {
        target: 'http://localhost:8001',
        rewrite: (path) => path.replace(/^\/agent/, ''),
      },
    },
  },
});
