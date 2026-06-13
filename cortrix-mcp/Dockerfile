# Cortrix MCP Server — stdio MCP server image.
#
# Build context = this directory (cortrix-mcp/). Installs the package from the local
# source so the image is buildable before the package is published to PyPI. Once
# cortrix-mcp is on PyPI, this can switch to `pip install cortrix-mcp==1.0.0` (feature
# design section 8.2).
#
#   docker build -t cortrix/mcp:v1.0.0 .
#   docker run -i --rm -e CORTRIX_URL=http://host.docker.internal:8080 \
#       -e CORTRIX_API_KEY=sk-cortrix-... cortrix/mcp:v1.0.0
#
# It speaks MCP over stdio, so run with -i (interactive stdin) and pipe the IDE client in.

FROM python:3.11-slim

WORKDIR /app

# Install build backend + the package itself from source.
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir .

# Default backend connection (override at runtime). Port 8080 matches cortrix-server.
ENV CORTRIX_URL=http://host.docker.internal:8080 \
    CORTRIX_NAMESPACE=default \
    CORTRIX_MCP_TIMEOUT=30

ENTRYPOINT ["cortrix-mcp"]
