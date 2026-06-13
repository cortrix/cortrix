#!/usr/bin/env python3
"""Vision LLM processor for OCR+LLM hybrid text extraction.

Takes a PDF file path, renders each page as JPEG, sends page image + OCR text
to a Vision LLM for structured text extraction.

Config via environment variables:
  CORTRIX_LLM_PROVIDER  - "anthropic" or "openai"
  CORTRIX_LLM_MODEL     - e.g. "claude-sonnet-4-20250514" or "gpt-4o"
  CORTRIX_LLM_API_KEY   - API key
  CORTRIX_LLM_BASE_URL  - Base URL (optional)
  CORTRIX_VLM_WORKERS   - parallel workers (default: 4)

Usage:
  python3 run_vision_llm.py <pdf_path> [--ocr-text <ocr_text_file>]

Output: JSON array [{text, confidence, box, page}], one entry per page.
"""
import sys
import os
import json
import base64
import tempfile
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed

PROMPT = (
    "Below is an image of one document page and the raw text extracted by OCR. "
    "Understand the content from the image, use the OCR text to ensure accuracy, "
    "filter out decorative/duplicate text, and output structured text content. "
    "Preserve meaningful information. "
    "Output only the extracted text content, without any explanation or prefix."
)

# Max image width for VLM — keep small to stay within free-tier size limits.
# Tests show 800px works reliably with GLM-4V Flash; 1600px triggers HTTP 400.
_VLM_MAX_WIDTH = 800
_VLM_JPEG_QUALITY = 75


def render_page_jpeg(doc, page_num, dpi=120, max_width=_VLM_MAX_WIDTH):
    """Render a PDF page to JPEG bytes."""
    page = doc[page_num]
    mat = __import__('fitz').Matrix(dpi / 72, dpi / 72)
    pix = page.get_pixmap(matrix=mat)

    from PIL import Image
    img = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
    del pix

    if img.width > max_width:
        ratio = max_width / img.width
        img = img.resize((max_width, int(img.height * ratio)), Image.LANCZOS)

    tmp = tempfile.mktemp(suffix='.jpg')
    img.save(tmp, 'JPEG', quality=_VLM_JPEG_QUALITY)
    with open(tmp, 'rb') as f:
        data = f.read()
    os.unlink(tmp)
    return data


def call_anthropic(image_b64, ocr_text, model, api_key, base_url):
    """Call Anthropic Messages API with vision."""
    import urllib.request

    url = (base_url.rstrip('/') if base_url else "https://api.anthropic.com") + "/v1/messages"

    user_text = PROMPT
    if ocr_text:
        user_text += f"\n\nRaw OCR text:\n{ocr_text}"

    body = {
        "model": model,
        "max_tokens": 1024,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg", "data": image_b64}},
                {"type": "text", "text": user_text},
            ]
        }]
    }

    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
        },
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        result = json.loads(resp.read())

    for block in result.get("content", []):
        if block.get("type") == "text":
            return block["text"]
    return ""


def call_openai(image_b64, ocr_text, model, api_key, base_url):
    """Call OpenAI Chat Completions API with vision."""
    import urllib.request
    import re

    # Normalize base_url: if it already ends with a version segment (/v1, /v4, etc.)
    # append only "/chat/completions"; otherwise add "/v1/chat/completions".
    # GLM uses "https://open.bigmodel.cn/api/paas/v4" (already versioned).
    # OpenAI uses "https://api.openai.com" (no version in base).
    base = (base_url.rstrip('/') if base_url else "https://api.openai.com")
    if re.search(r'/v\d+$', base):
        url = base + "/chat/completions"
    else:
        url = base + "/v1/chat/completions"

    user_text = PROMPT
    if ocr_text:
        user_text += f"\n\nRaw OCR text:\n{ocr_text}"

    body = {
        "model": model,
        "max_tokens": 1024,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}},
                {"type": "text", "text": user_text},
            ]
        }]
    }

    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            result = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        err_body = e.read().decode(errors='replace')[:300]
        raise RuntimeError(f"HTTP {e.code}: {err_body}") from e

    choices = result.get("choices", [])
    if choices:
        return choices[0].get("message", {}).get("content", "")
    return ""


def call_vlm_page(args):
    """Call VLM for a single pre-rendered page. Runs in thread pool (no fitz access)."""
    page_num, total, image_b64, ocr_text, call_fn, model, api_key, base_url = args
    print(f"[VLM] page {page_num+1}/{total}", file=sys.stderr, flush=True)
    try:
        text = call_fn(image_b64, ocr_text, model, api_key, base_url)
    except Exception as e:
        print(f"[VLM] page {page_num+1} failed: {e}", file=sys.stderr, flush=True)
        text = ocr_text  # fallback to raw OCR text
    return page_num, text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pdf_path", help="Path to PDF file")
    parser.add_argument("--ocr-text", help="Path to file containing OCR text (\\f separated pages)")
    args = parser.parse_args()

    provider = os.environ.get("CORTRIX_LLM_PROVIDER", "")
    model = os.environ.get("CORTRIX_LLM_MODEL", "")
    api_key = os.environ.get("CORTRIX_LLM_API_KEY", "")
    base_url = os.environ.get("CORTRIX_LLM_BASE_URL", "")
    workers = int(os.environ.get("CORTRIX_VLM_WORKERS", "4"))

    if not provider or not api_key or not model:
        print(json.dumps({"error": "Missing LLM config (CORTRIX_LLM_PROVIDER/MODEL/API_KEY)"}),
              file=sys.stderr)
        return 1

    call_fn = call_anthropic if provider == "anthropic" else call_openai

    # Load OCR text (pages separated by \f)
    ocr_pages = []
    if args.ocr_text and os.path.exists(args.ocr_text):
        with open(args.ocr_text, 'r', encoding='utf-8') as f:
            ocr_pages = f.read().split('\f')

    import fitz
    doc = fitz.open(args.pdf_path)
    total = len(doc)

    # Step 1: Render all pages in the main thread.
    # PyMuPDF (fitz) is NOT thread-safe — must not share doc across threads.
    print(f"[VLM] rendering {total} pages", file=sys.stderr, flush=True)
    page_args = []
    for i in range(total):
        jpeg_data = render_page_jpeg(doc, i)
        image_b64 = base64.b64encode(jpeg_data).decode()
        ocr_text = ocr_pages[i].strip() if i < len(ocr_pages) else ""
        page_args.append((i, total, image_b64, ocr_text, call_fn, model, api_key, base_url))
    doc.close()

    # Step 2: VLM API calls in parallel (no fitz access, pure HTTP).
    print(f"[VLM] calling LLM for {total} pages with {workers} workers", file=sys.stderr, flush=True)
    results = {}
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(call_vlm_page, args): args[0] for args in page_args}
        for future in as_completed(futures):
            page_num, text = future.result()
            if text and text.strip():
                results[page_num] = text

    # Output in page order
    all_lines = [
        {
            "text": results[i].strip(),
            "confidence": 0.95,
            "box": [[0, 0], [0, 0], [0, 0], [0, 0]],
            "page": i,
        }
        for i in range(total) if i in results
    ]
    print(json.dumps(all_lines, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
