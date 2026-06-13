#!/usr/bin/env python3
"""Convert BAAI/bge-reranker-v2-m3 (HF safetensors) to ONNX for OnnxReranker.

Usage:
    python3 scripts/convert_reranker_to_onnx.py <hf_model_dir> <out_dir>

<hf_model_dir> must contain config.json + model.safetensors + tokenizer.json
(download from the HF repo). Produces <out_dir>/model.onnx (external weight
file model.onnx_data alongside when the graph exceeds the 2GB protobuf limit)
and copies tokenizer.json next to it — the layout OnnxReranker expects via
RerankerConfig.model_path / tokenizer_path.

PITFALL: do NOT load the model with low_cpu_mem_usage=True (or device_map).
That leaves the weights on the meta device and torch.onnx.export silently
serializes an EMPTY graph (~9MB instead of ~2.1GB). Load fully materialized
fp32 weights, eval mode, then export.
"""
import shutil
import sys
from pathlib import Path

import onnx
import torch
from transformers import AutoModelForSequenceClassification


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    # Full fp32 materialization — see PITFALL above.
    model = AutoModelForSequenceClassification.from_pretrained(
        src, torch_dtype=torch.float32
    )
    model.eval()

    # Export to a temp subdir: >2GB models make the TorchScript exporter spill
    # every tensor into its own loose external file (170+ files); we export
    # there, then re-save consolidated into <out>.
    tmp = out / "_export_tmp"
    tmp.mkdir(exist_ok=True)
    ids = torch.ones(1, 16, dtype=torch.long)
    mask = torch.ones(1, 16, dtype=torch.long)
    torch.onnx.export(
        model,
        (ids, mask),
        str(tmp / "model.onnx"),
        input_names=["input_ids", "attention_mask"],
        output_names=["logits"],
        dynamic_axes={
            "input_ids": {0: "batch", 1: "seq"},
            "attention_mask": {0: "batch", 1: "seq"},
            "logits": {0: "batch"},
        },
        opset_version=17,
    )
    del model  # free the torch copy before materializing the proto

    # Consolidate: model.onnx (graph) + model.onnx_data (single weight file) —
    # the layout OnnxReranker/bge-m3 use.
    proto = onnx.load(str(tmp / "model.onnx"), load_external_data=True)
    onnx.save_model(
        proto,
        str(out / "model.onnx"),
        save_as_external_data=True,
        all_tensors_to_one_file=True,
        location="model.onnx_data",
        size_threshold=1024,
    )
    shutil.rmtree(tmp)
    shutil.copy(src / "tokenizer.json", out / "tokenizer.json")

    onnx_size = (out / "model.onnx").stat().st_size
    data = out / "model.onnx_data"
    total = onnx_size + (data.stat().st_size if data.exists() else 0)
    print(f"model.onnx={onnx_size} bytes, total with external data={total} bytes")
    if total < 2_000_000_000:
        print("ERROR: export looks too small — meta-device empty-weight pitfall?")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
