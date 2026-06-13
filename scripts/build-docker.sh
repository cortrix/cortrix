#!/bin/bash
# Build Cortrix Docker image
# Usage: ./scripts/build-docker.sh [tag]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TAG="${1:-latest}"
IMAGE_NAME="cortrix"

echo "========================================"
echo "  Building Cortrix Docker Image"
echo "  Tag: ${IMAGE_NAME}:${TAG}"
echo "========================================"

cd "$PROJECT_DIR"

# Record build start time
START_TIME=$(date +%s)

# Build the image
docker build \
    -t "${IMAGE_NAME}:${TAG}" \
    -f deploy/Dockerfile \
    --build-arg BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --build-arg VERSION="${TAG}" \
    .

# Record build end time
END_TIME=$(date +%s)
BUILD_DURATION=$((END_TIME - START_TIME))

# Get image size
IMAGE_SIZE=$(docker image inspect "${IMAGE_NAME}:${TAG}" --format='{{.Size}}' | awk '{printf "%.1f MB", $1/1024/1024}')

echo ""
echo "========================================"
echo "  Build Complete!"
echo "  Image: ${IMAGE_NAME}:${TAG}"
echo "  Size:  ${IMAGE_SIZE}"
echo "  Time:  ${BUILD_DURATION}s"
echo "========================================"
echo ""
echo "Quick start:"
echo "  docker run -d --name cortrix -p 8080:8080 -v ./data:/data ${IMAGE_NAME}:${TAG}"
