# ==========================================================
# Cortrix — Semantic Storage for the Agentic Era
# Multi-stage Dockerfile
#
# Build:  docker build -t cortrix:latest -f deploy/Dockerfile ..
# Run:    docker run -d -p 8420:8420 -v ./data:/data \
#           -e CORTRIX_LLM_API_KEY=sk-xxx cortrix:latest
# ==========================================================

# ========== Stage 1: C++ Builder ==========
FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    wget \
    curl \
    pkg-config \
    libssl-dev \
    libsqlite3-dev \
    libpq-dev \
    python3.10 \
    python3.10-dev \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# cmake >= 3.24 required: Dependencies.cmake uses DOWNLOAD_EXTRACT_TIMESTAMP
# (jammy's apt cmake is 3.22, which mis-parses that keyword as a URL entry)
RUN pip3 install --no-cache-dir "cmake>=3.27"

WORKDIR /build

# Copy CMake infrastructure first for better layer caching
COPY cmake          cortrix/cmake/
COPY CMakeLists.txt cortrix/CMakeLists.txt

# Copy cortrix OSS source (tests/ required: root CMakeLists add_subdirectory
# is unconditional; BUILD_TESTS=OFF only skips the targets)
COPY include     cortrix/include/
COPY src         cortrix/src/
COPY tests       cortrix/tests/

# Build cortrix-server
RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_TESTS=OFF \
          ../cortrix && \
    make -j$(nproc) cortrix-server && \
    strip cortrix-server

# Pre-build Python wheels for runtime (cortrix-agent lives at repo top level).
# PIP_INDEX_URL is overridable per build (e.g. a regional mirror when the
# default route to pypi is flaky); generous timeout/retries either way.
ARG PIP_INDEX_URL=https://pypi.org/simple
ENV PIP_INDEX_URL=${PIP_INDEX_URL} \
    PIP_TIMEOUT=120 \
    PIP_RETRIES=5
# No existence guard: requirements.txt is always expected, and a silent skip
# here ships an image whose cortrix-agent crash-loops (missing uvicorn).
# Python sources are COPYed after the C++ build on purpose: they take no part
# in the cmake step, and copying them earlier invalidated the (expensive)
# compile layer on every Python-side change.
# sdk/python is the P03 SDK — cortrix-agent imports it as a sibling package
# (main.py resolves ../sdk/python), so it ships as source, not a wheel.
COPY cortrix-agent  cortrix/cortrix-agent/
COPY sdk/python     cortrix/sdk/python/
RUN mkdir -p /wheels && \
    pip3 wheel --no-cache-dir --wheel-dir /wheels \
        -r cortrix/cortrix-agent/requirements.txt

# ========== Stage 2: Web UI Builder ==========
# node 20: react-router-dom@7 requires >=20. NPM_REGISTRY overridable per
# build (regional mirror when the default route is flaky).
FROM node:20-slim AS web-builder

ARG NPM_REGISTRY=https://registry.npmjs.org

WORKDIR /build

# Copy web UI source (repo dir is web/; vite outDir is `static`, normalize to dist)
COPY web/ ./web-ui/

RUN if [ -f "web-ui/package.json" ]; then \
        cd web-ui && \
        npm config set registry ${NPM_REGISTRY} && \
        npm ci --ignore-scripts --fetch-retries 5 --fetch-retry-maxtimeout 120000 && \
        npm run build && \
        rm -rf dist && mv static dist; \
    else \
        mkdir -p web-ui/dist && \
        echo '<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">' \
             '<meta name="viewport" content="width=device-width,initial-scale=1.0">' \
             '<title>Cortrix</title>' \
             '<style>body{font-family:system-ui;display:flex;justify-content:center;' \
             'align-items:center;min-height:100vh;margin:0;background:#0a0a0a;color:#e0e0e0}' \
             '.c{text-align:center}h1{font-size:2.5rem;margin-bottom:.5rem}' \
             'p{color:#888;font-size:1.1rem}a{color:#6cb4ee}</style></head>' \
             '<body><div class="c"><h1>Cortrix</h1>' \
             '<p>Semantic Storage for the Agentic Era</p>' \
             '<p style="margin-top:2rem"><a href="/api/v1/health">/api/v1/health</a>' \
             ' &middot; <a href="/api/v1/namespaces">/api/v1/namespaces</a></p>' \
             '</div></body></html>' > web-ui/dist/index.html; \
    fi

# ========== Stage 3: Runtime ==========
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

LABEL maintainer="Cortrix Team <team@cortrix.ai>"
LABEL description="Cortrix — Semantic Storage for the Agentic Era"
LABEL version="1.0.0"

# Runtime dependencies only — no compilers
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libsqlite3-0 \
    libpq5 \
    python3.10 \
    python3-pip \
    supervisor \
    curl \
    ca-certificates \
    tzdata \
    && rm -rf /var/lib/apt/lists/*

# Non-root user
RUN useradd -m -u 1000 -s /bin/bash cortrix

# Directory layout
RUN mkdir -p /app /app/config /app/web-ui /app/python /data /data/blobs /data/p-hnsw /data/logs && \
    chown -R cortrix:cortrix /app /data

WORKDIR /app

# --- Copy artifacts from builder stages ---

# C++ server binary + the ONNX Runtime shared lib it links against
# (FetchContent ships a prebuilt .so; without it the server exits 127)
COPY --from=builder /build/build/cortrix-server /app/cortrix_server
COPY --from=builder /build/build/_deps/onnxruntime-src/lib/libonnxruntime.so* /usr/local/lib/
RUN ldconfig

# Default YAML config + F20 log-sanitizer rules (main.cpp reads the fixed
# path /app/config/sensitive_fields.yaml)
COPY deploy/cortrix.yaml              /app/config/cortrix.yaml
COPY src/logging/sensitive_fields.yaml /app/config/sensitive_fields.yaml

# Web UI static files
COPY --from=web-builder /build/web-ui/dist /app/web-ui

# Python code + pre-built wheels (image layout keeps /app/python/cortrix-agent
# with the P03 SDK as its sibling at /app/python/sdk/python — main.py resolves
# parents[1]/sdk/python; supervisord.conf depends on this layout)
COPY --from=builder /build/cortrix/cortrix-agent /app/python/cortrix-agent
COPY --from=builder /build/cortrix/sdk/python    /app/python/sdk/python
COPY --from=builder /wheels /tmp/wheels

# F06 parser bridge scripts (auto-located at /app/scripts by the server).
# txt/md parse via the bridge's plain-text fast path with zero extra deps;
# the heavy docling/paddleocr stacks install only for the real-model pass
# (D-R3 #483: pip install -r /app/scripts/requirements-parser.txt).
COPY scripts/docling_bridge.py \
     scripts/paddleocr_bridge.py \
     scripts/requirements-parser.txt /app/scripts/

# Install Python deps from wheels (fast, no compilation)
# Fail the build loudly if the cortrix-agent deps cannot install — a silent
# failure here ships an image whose cortrix-agent crash-loops at startup.
RUN pip3 install --no-cache-dir /tmp/wheels/*.whl && rm -rf /tmp/wheels

# --- Deploy-time config files ---
COPY deploy/supervisord.conf  /etc/supervisor/conf.d/cortrix.conf
COPY deploy/entrypoint.sh     /app/entrypoint.sh
COPY deploy/healthcheck.sh    /app/healthcheck.sh

RUN chmod +x /app/entrypoint.sh /app/healthcheck.sh && \
    chown -R cortrix:cortrix /app

# --- Runtime settings ---
USER cortrix
EXPOSE 8420
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
    CMD /app/healthcheck.sh

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["start"]
