#!/bin/bash
# Push Cortrix Docker image to registry
# Usage: ./scripts/push-docker.sh [tag] [registry]

set -e

TAG="${1:-latest}"
REGISTRY="${2:-cortrix}"
IMAGE_NAME="cortrix"
FULL_NAME="${REGISTRY}/${IMAGE_NAME}:${TAG}"

echo "========================================"
echo "  Pushing Cortrix Docker Image"
echo "  Target: ${FULL_NAME}"
echo "========================================"

# Tag for registry
docker tag "${IMAGE_NAME}:${TAG}" "${FULL_NAME}"

# Push
docker push "${FULL_NAME}"

echo ""
echo "Push complete: ${FULL_NAME}"
echo ""
echo "Pull command:"
echo "  docker pull ${FULL_NAME}"
