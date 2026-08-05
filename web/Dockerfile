# Cortrix Web UI — multi-stage build (web UI design § 11.1 / S9).
#
# AUTHORITATIVE SoT for the full image (cortrix-server binary stage +
# multi-arch build-args + alpine security baseline) is deployment
# (the deployment Dockerfile design) and readiness (SPA-fallback +
# static-asset security headers). This Dockerfile is the web-UI-side web-builder
# stage + a reference assembly so the UI is buildable/embeddable standalone; the
# production pipeline composes the `web-builder` stage below into the deployment server
# image via `COPY --from`.
#
# Layout (§ 11.1 / § 11.2):
#   GET /            -> /cortrix-web/index.html
#   GET /assets/*    -> static files (gzip + brotli + Cache-Control)  [readiness]
#   any non-/api/*   -> SPA fallback to index.html                    [readiness]
# cortrix-server is started with `--web-root=/cortrix-web` (deployment).

# ── Stage 1: web-builder ────────────────────────────────────────────────────
# Pinned node:20-alpine (matches § 11.1 sketch + the base-image baseline base-image baseline).
FROM node:20-alpine AS web-builder
WORKDIR /app

# Install deps against the lockfile (reproducible). npm ci needs both manifests.
COPY package.json package-lock.json ./
# --legacy-peer-deps: the React 18 / RTL 16 peer graph (matches local install).
RUN npm ci --legacy-peer-deps

# Build the SPA → static/ (Vite outDir, see vite.config.ts).
COPY . .
RUN npm run build

# ── Stage 2: web-dist (export-only artifact) ─────────────────────────────────
# Minimal stage exposing just the built assets for the deployment server image to copy:
#   COPY --from=cortrix-web /cortrix-web /cortrix-web
# Kept distro-less-ish (scratch) so this artifact carries no runtime surface.
FROM scratch AS web-dist
COPY --from=web-builder /app/static /cortrix-web

# ── Stage 3 (reference): assembled server image ──────────────────────────────
# The REAL final stage lives in deployment (cortrix-server binary + alpine 3.19 base +
# security headers + SPA fallback). Shown here commented as the integration
# contract — do not duplicate the server build here (single SoT in deployment):
#
#   FROM alpine:3.19
#   COPY --from=<cortrix-server-build> /usr/local/bin/cortrix-server /usr/local/bin/
#   COPY --from=web-builder /app/static /cortrix-web
#   EXPOSE 8080 9091
#   CMD ["cortrix-server", "--web-root=/cortrix-web"]
