#!/usr/bin/env bash
# ==============================================================================
# TensorRT Engine Builder for AI Video Super Resolution on Blackwell (RTX 5070)
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/engines"
mkdir -p "${OUTPUT_DIR}"

MODELS_DIR="/home/eegor/Проекты/webgpu-upscale-extension/ai/models"

if [ ! -d "${MODELS_DIR}" ]; then
    echo "Models directory not found at ${MODELS_DIR}"
    exit 1
fi

echo "=== Building TensorRT Engines for RTX 5070 ==="

# 1. ESPCN x3 (Ultra-fast real-time: 0.28 ms, 3400+ FPS)
echo "[1/2] Compiling ESPCN x3..."
trtexec \
    --onnx="${MODELS_DIR}/espcn-x3.onnx" \
    --saveEngine="${OUTPUT_DIR}/espcn-x3.engine"

echo "[1/2] ESPCN x3 engine built successfully: ${OUTPUT_DIR}/espcn-x3.engine"

# 2. Real-ESRGAN x4 (High quality reconstruction)
echo "[2/2] Compiling Real-ESRGAN x4 with dynamic shapes..."
trtexec \
    --onnx="${MODELS_DIR}/realesr-general-x4v3.onnx" \
    --minShapes=input:1x3x256x256 \
    --optShapes=input:1x3x720x1280 \
    --maxShapes=input:1x3x1080x1920 \
    --saveEngine="${OUTPUT_DIR}/realesr-general-x4v3.engine"

echo "[2/2] Real-ESRGAN x4 engine built successfully: ${OUTPUT_DIR}/realesr-general-x4v3.engine"
echo "All engines compiled and ready!"
