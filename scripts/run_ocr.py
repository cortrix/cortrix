#!/usr/bin/env python3
"""OCR processor using PaddleOCR.
Handles both image files and PDF files (page-by-page to avoid OOM).
Outputs JSON: [{text, confidence, box}]

Requires: paddlepaddle + paddleocr + PyMuPDF
  (installed in scripts/ocr_venv with Python 3.12)
"""
import sys
import json
import os
import tempfile
import gc

# The caller parses this script's stdout as JSON, but paddlepaddle's native
# core printf()s diagnostic lines straight to fd 1 (observed on 3.1/3.2).
# Reserve the real stdout for the JSON result and repoint fd 1 at stderr for
# everything else — native writes bypass sys.stdout, so a Python-level
# redirect is not enough.
_RESULT_FD = os.dup(1)
os.dup2(2, 1)

os.environ["PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK"] = "True"

from paddleocr import PaddleOCR

# Initialize once (lazy singleton)
_ocr_engine = None


def _get_engine():
    global _ocr_engine
    if _ocr_engine is None:
        _ocr_engine = PaddleOCR(
            use_textline_orientation=True,
            lang="ch",  # Chinese + English
        )
    return _ocr_engine


def _detect_ext(file_path):
    """Detect file extension from magic bytes if path has no extension."""
    ext = os.path.splitext(file_path)[1].lower()
    if ext in ('.pdf', '.png', '.jpg', '.jpeg', '.bmp'):
        return ext
    with open(file_path, 'rb') as f:
        header = f.read(8)
    if header[:4] == b'%PDF':
        return '.pdf'
    if header[:8] == b'\x89PNG\r\n\x1a\n':
        return '.png'
    if header[:2] == b'\xff\xd8':
        return '.jpg'
    if header[:2] == b'BM':
        return '.bmp'
    return '.pdf'


def _extract_lines(results):
    """Extract text lines from PaddleOCR predict() results."""
    all_lines = []
    if results is None:
        return all_lines

    for page_result in results:
        rec_texts = page_result.get("rec_texts", [])
        rec_scores = page_result.get("rec_scores", [])
        dt_polys = page_result.get("dt_polys", [])
        rec_boxes = page_result.get("rec_boxes", [])

        boxes = dt_polys if dt_polys is not None and len(dt_polys) > 0 else rec_boxes

        for i, text in enumerate(rec_texts):
            if not text or not text.strip():
                continue
            confidence = float(rec_scores[i]) if i < len(rec_scores) else 0.0
            if boxes is not None and i < len(boxes):
                box = boxes[i]
                if hasattr(box, 'tolist'):
                    box = box.tolist()
                if len(box) == 4 and not isinstance(box[0], (list, tuple)):
                    x1, y1, x2, y2 = [int(v) for v in box]
                    int_box = [[x1, y1], [x2, y1], [x2, y2], [x1, y2]]
                else:
                    int_box = [[int(p[0]), int(p[1])] for p in box]
            else:
                int_box = [[0, 0], [0, 0], [0, 0], [0, 0]]

            all_lines.append({
                "text": text,
                "confidence": round(confidence, 4),
                "box": int_box,
            })
    return all_lines


def _ocr_pdf_paged(file_path, engine, dpi=120, max_width=1600):
    """OCR a PDF page-by-page to avoid OOM on large files."""
    import fitz  # PyMuPDF
    from PIL import Image

    doc = fitz.open(file_path)
    all_lines = []

    total_pages = len(doc)
    for page_num in range(total_pages):
        page = doc[page_num]
        mat = fitz.Matrix(dpi / 72, dpi / 72)
        pix = page.get_pixmap(matrix=mat)
        img = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
        del pix

        # Resize large images to cap memory usage
        if img.width > max_width:
            ratio = max_width / img.width
            img = img.resize((max_width, int(img.height * ratio)), Image.LANCZOS)

        tmp_img = tempfile.mktemp(suffix='.jpg')
        try:
            img.save(tmp_img, 'JPEG', quality=85)
            print(f"[OCR] page {page_num+1}/{total_pages} ({img.width}x{img.height})",
                  file=sys.stderr, flush=True)
            results = engine.predict(tmp_img)
            page_lines = _extract_lines(results)
            for pl in page_lines:
                pl["page"] = page_num  # 0-based page number
            all_lines.extend(page_lines)
            print(f"[OCR] page {page_num+1}/{total_pages} done, {len(page_lines)} lines",
                  file=sys.stderr, flush=True)
        finally:
            if os.path.exists(tmp_img):
                os.unlink(tmp_img)
            del img
            gc.collect()

    doc.close()
    return all_lines


def run_ocr(file_path):
    try:
        engine = _get_engine()

        # Detect file type
        ext = os.path.splitext(file_path)[1].lower()
        if ext not in ('.pdf', '.png', '.jpg', '.jpeg', '.bmp'):
            ext = _detect_ext(file_path)

        # For PDFs: page-by-page OCR to avoid OOM
        if ext == '.pdf':
            # Ensure file has .pdf extension for fitz
            if not file_path.lower().endswith('.pdf'):
                tmp_path = tempfile.mktemp(suffix='.pdf')
                os.symlink(os.path.abspath(file_path), tmp_path)
                try:
                    all_lines = _ocr_pdf_paged(tmp_path, engine)
                finally:
                    os.unlink(tmp_path)
            else:
                all_lines = _ocr_pdf_paged(file_path, engine)
        else:
            # For images: direct OCR
            actual_path = file_path
            tmp_path = None
            if ext not in ('.png', '.jpg', '.jpeg', '.bmp'):
                tmp_path = tempfile.mktemp(suffix=ext)
                os.symlink(os.path.abspath(file_path), tmp_path)
                actual_path = tmp_path
            try:
                results = engine.predict(actual_path)
                all_lines = _extract_lines(results)
            finally:
                if tmp_path and os.path.exists(tmp_path):
                    os.unlink(tmp_path)

        os.write(_RESULT_FD, (json.dumps(all_lines, ensure_ascii=False) + "\n").encode("utf-8"))
        return 0
    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(run_ocr(sys.argv[1]) if len(sys.argv) >= 2 else 1)
