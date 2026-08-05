#!/usr/bin/env python3
"""PaddleOCR fallback bridge for PaddleOCRParser.

Invoked as a subprocess by cortrix::spc::PaddleOCRParser:

    python3 paddleocr_bridge.py --filepath FILE --timeout SECS \
        --max-pages N --lang ch --output-format json [--use-gpu]

Emits the parser page-level JSON protocol on stdout (same envelope as
docling_bridge.py). OCR specifics: page_metadata.parser_used = "paddleocr",
every paragraph type = TEXT, section = "" (OCR can't recover structure), and
paragraph.confidence is the OCR score. PDFs are OCR'd page-by-page; images
become a single page.

On expected failures the envelope carries a non-zero `status` (matching
ParserError) + the GEN-Agent 4 fields, and the process exits 0 (the C++ side
reads the structured error). Unexpected exceptions exit non-zero → the C++ side
maps to OCR_FAILED.

D3 standalone: paddleocr need not be installed — this reports SUBPROCESS_FAILED
cleanly and the C++ wrapper logic is unit-tested against a mock bridge. Real OCR
end-to-end is D3.5.
"""
import argparse
import json
import os
import sys
import time

OK = 0
FILE_NOT_FOUND = 1
UNSUPPORTED_FORMAT = 3
SUBPROCESS_FAILED = 5
INVALID_OUTPUT = 7
EMPTY_DOCUMENT = 9
OCR_FAILED = 11

_IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".tiff", ".tif", ".bmp")


def _emit(obj):
    json.dump(obj, sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")
    sys.stdout.flush()


def _error(status, msg, *, retryable=False, category="PERMANENT",
           retry_after_ms=0, structured_data=None):
    return {
        "status": status,
        "parser": "paddleocr",
        "metadata": {},
        "pages": [],
        "failed_pages": [],
        "error_msg": msg,
        "retryable": retryable,
        "category": category,
        "retry_after_ms": retry_after_ms,
        "structured_data": structured_data or {},
    }


def _page(page_num, lines, lang):
    """Build one §3.1 page from OCR lines [{text, confidence}]."""
    paras = []
    char_count = 0
    for ln in lines:
        text = ln.get("text", "")
        if not text:
            continue
        paras.append({
            "text": text,
            "page": page_num,
            "section": "",                 # OCR has no structure
            "type": "TEXT",                # OCR can't tell tables/captions apart
            "confidence": float(ln.get("confidence", 0.0)),
            "language": lang or "",
            "char_offset": -1,
            "char_length": len(text),
        })
        char_count += len(text)
    avg = sum(p["confidence"] for p in paras) / len(paras) if paras else 0.0
    return {
        "page_num": page_num,
        "page_text": "\n".join(p["text"] for p in paras),
        "page_metadata": {
            "page_num": page_num,
            "parser_used": "paddleocr",
            "page_confidence": avg,
            "is_scan_page": True,
            "char_count": char_count,
        },
        "paragraphs": paras,
    }


def run_ocr(filepath, max_pages, lang, use_gpu):
    if not os.path.isfile(filepath):
        return _error(FILE_NOT_FOUND, "File not found: %s" % filepath,
                      structured_data={"code": "CX_ERR_FILE_NOT_FOUND",
                                       "filepath": filepath})

    ext = os.path.splitext(filepath)[1].lower()
    if ext not in (".pdf",) + _IMAGE_EXTS:
        return _error(UNSUPPORTED_FORMAT, "PaddleOCR cannot process %s" % ext,
                      structured_data={"code": "CX_ERR_UNSUPPORTED_FORMAT",
                                       "extension": ext.lstrip("."),
                                       "supported_formats": ["pdf", "png", "jpg",
                                                              "tiff", "bmp"]})

    try:
        from paddleocr import PaddleOCR
    except ImportError as e:
        return _error(SUBPROCESS_FAILED, "paddleocr not installed: %s" % e,
                      structured_data={"code": "CX_ERR_SUBPROCESS_FAILED",
                                       "hint": "pip install -r requirements-parser.txt"})

    start = time.time()
    engine = PaddleOCR(use_textline_orientation=True, lang=lang or "ch")

    pages = []
    if ext == ".pdf":
        # Page-by-page to avoid OOM (mirrors the MVP run_ocr.py approach).
        import fitz  # PyMuPDF
        from PIL import Image
        import tempfile
        doc = fitz.open(filepath)
        total = len(doc)
        for i in range(total):
            if max_pages and i >= max_pages:
                break
            pix = doc[i].get_pixmap(matrix=fitz.Matrix(120 / 72, 120 / 72))
            img = Image.frombytes("RGB", [pix.width, pix.height], pix.samples)
            tmp = tempfile.mktemp(suffix=".jpg")
            try:
                img.save(tmp, "JPEG", quality=85)
                results = engine.predict(tmp)
                lines = _ocr_lines(results)
            finally:
                if os.path.exists(tmp):
                    os.unlink(tmp)
            pages.append(_page(i + 1, lines, lang))
        doc.close()
    else:
        results = engine.predict(filepath)
        pages.append(_page(1, _ocr_lines(results), lang))

    return {
        "status": OK if any(p["paragraphs"] for p in pages) else EMPTY_DOCUMENT,
        "parser": "paddleocr",
        "metadata": {
            "filename": os.path.basename(filepath),
            "doc_title": "",
            "mime_type": "",
            "file_size_bytes": os.path.getsize(filepath),
            "page_count": len(pages),
            "doc_language": lang or "",
            "upload_timestamp": int(time.time() * 1000),
            "parse_time_ms": int((time.time() - start) * 1000),
        },
        "pages": pages,
        "failed_pages": [],
        "error_msg": "",
        "retryable": False,
        "category": "NONE",
        "retry_after_ms": 0,
        "structured_data": {},
    }


def _ocr_lines(results):
    """Flatten PaddleOCR predict() results to [{text, confidence}]."""
    out = []
    if not results:
        return out
    for page in results:
        rec_texts = page.get("rec_texts", [])
        rec_scores = page.get("rec_scores", [])
        for i, text in enumerate(rec_texts):
            if not text or not text.strip():
                continue
            score = float(rec_scores[i]) if i < len(rec_scores) else 0.0
            out.append({"text": text, "confidence": round(score, 4)})
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description="PaddleOCR parser bridge (F06)")
    parser.add_argument("--filepath", required=True)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--max-pages", type=int, default=200)
    parser.add_argument("--lang", default="ch")
    parser.add_argument("--output-format", default="json")
    parser.add_argument("--use-gpu", action="store_true")
    args = parser.parse_args(argv)

    if args.output_format != "json":
        _emit(_error(INVALID_OUTPUT,
                     "unsupported output format: %s" % args.output_format,
                     structured_data={"code": "CX_ERR_INVALID_OUTPUT",
                                      "parse_error_pos": -1}))
        return 0

    result = run_ocr(args.filepath, args.max_pages, args.lang, args.use_gpu)
    _emit(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
