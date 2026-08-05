#!/usr/bin/env python3
"""Docling document-parser bridge for DoclingParser.

Invoked as a subprocess by cortrix::spc::DoclingParser:

    python3 docling_bridge.py --filepath FILE --timeout SECS \
        --max-pages N --language-hint LANG --output-format json

Emits the parser page-level JSON protocol on stdout:

    {"status": 0, "parser": "docling",
     "metadata": {filename, doc_title, mime_type, file_size_bytes,
                  page_count, doc_language, upload_timestamp, parse_time_ms},
     "pages": [{"page_num", "page_text", "page_metadata": {...},
                "paragraphs": [{text, page, section, type, confidence,
                                language, char_offset, char_length}, ...]}, ...],
     "failed_pages": [], "error_msg": "",
     "retryable": false, "category": "NONE", "retry_after_ms": 0,
     "structured_data": {}}

On error, emits the same envelope with a non-zero `status` (matching
ParserError in parser_errors.h) and the GEN-Agent 4 fields
(retryable / category / retry_after_ms / structured_data). The process still
exits 0 in that case so the C++ side reads the structured error from stdout
rather than treating it as a crash; genuine crashes (uncaught) exit non-zero
and the C++ side maps them to SUBPROCESS_CRASHED.

D3 standalone note: real `docling` need not be installed on the dev machine —
this script reports MISSING_DEPENDENCY cleanly, and DoclingParser wrapper logic
is unit-tested against a mock bridge. Real docling end-to-end is D3.5.
"""
import argparse
import json
import os
import sys
import time

# ParserError integer codes (mirror parser_errors.h §2.7).
OK = 0
FILE_NOT_FOUND = 1
PARSE_TIMEOUT = 4
SUBPROCESS_FAILED = 5
INVALID_OUTPUT = 7
EMPTY_DOCUMENT = 9
PASSWORD_PROTECTED = 13
CORRUPTED_FILE = 14


def _emit(obj):
    """Write one JSON object to stdout (UTF-8, no ASCII escaping)."""
    json.dump(obj, sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")
    sys.stdout.flush()


def _error(status, msg, *, retryable=False, category="PERMANENT",
           retry_after_ms=0, structured_data=None):
    return {
        "status": status,
        "parser": "docling",
        "metadata": {},
        "pages": [],
        "failed_pages": [],
        "error_msg": msg,
        "retryable": retryable,
        "category": category,
        "retry_after_ms": retry_after_ms,
        "structured_data": structured_data or {},
    }


def _classify_type(element):
    """Map a Docling element to a parser ChunkType string."""
    label = (getattr(element, "label", "") or "").lower()
    if "table" in label:
        return "TABLE"
    if "code" in label:
        return "CODE"
    if "caption" in label or "picture" in label or "figure" in label:
        return "IMAGE_CAPTION"
    return "TEXT"


def _parent_section(element):
    """Best-effort section heading for an element (empty if unknown)."""
    parent = getattr(element, "parent", None)
    if parent is not None:
        label = (getattr(parent, "label", "") or "").lower()
        if "header" in label or "title" in label or "section" in label:
            return getattr(parent, "text", "") or ""
    return ""


def _parse_plain_text(filepath, max_pages, lang_hint):
    """txt/md fast path: one synthetic page, blank-line paragraph split."""
    start = time.time()
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            raw = f.read()
    except OSError as e:
        return _error(CORRUPTED_FILE, "Cannot read file: %s" % e,
                      structured_data={"code": "CX_ERR_CORRUPTED_FILE",
                                       "magic_bytes_expected": "",
                                       "magic_bytes_actual": ""})

    chunks = [c.strip() for c in raw.split("\n\n")]
    paras = []
    offset = 0
    for c in chunks:
        if c:
            paras.append({
                "text": c,
                "page": 1,
                "section": "",
                "type": "paragraph",
                "confidence": 1.0,
                "language": lang_hint or "",
                "char_offset": -1,  # offsets are a downstream extension
                "char_length": len(c),
            })
        offset += len(c) + 2

    pages = []
    if paras:
        pages.append({
            "page_num": 1,
            "page_text": "\n".join(p["text"] for p in paras),
            "page_metadata": {
                "page_num": 1,
                "parser_used": "plaintext",
                "page_confidence": 1.0,
                "is_scan_page": False,
                "char_count": sum(p["char_length"] for p in paras),
            },
            "paragraphs": paras,
        })

    filename = os.path.basename(filepath)
    try:
        size = os.path.getsize(filepath)
    except OSError:
        size = 0

    return {
        "status": OK if pages else EMPTY_DOCUMENT,
        "parser": "plaintext",
        "metadata": {
            "filename": filename,
            "doc_title": "",
            "mime_type": "text/markdown" if ext_is_md(filepath) else "text/plain",
            "file_size_bytes": size,
            "page_count": len(pages),
            "doc_language": lang_hint or "",
            "upload_timestamp": int(time.time() * 1000),
            "parse_time_ms": int((time.time() - start) * 1000),
        },
        "pages": pages,
        "failed_pages": [],
        "error_msg": "" if pages else "Document produced no text",
        "retryable": False,
        "category": "NONE",
        "retry_after_ms": 0,
        "structured_data": {} if pages else {"empty_reason": "all_pages_blank"},
    }


def ext_is_md(filepath):
    return os.path.splitext(filepath)[1].lower() in (".md", ".markdown")


def parse_document(filepath, timeout, max_pages, lang_hint):
    """Drive Docling and assemble the §3.1 page-level envelope.

    Returns the JSON-able dict (never raises for expected conditions; unexpected
    exceptions propagate to main() which maps them to a crash exit code).
    """
    if not os.path.isfile(filepath):
        return _error(FILE_NOT_FOUND, "File not found: %s" % filepath,
                      structured_data={"code": "CX_ERR_FILE_NOT_FOUND",
                                       "filepath": filepath})

    # Plain-text fast path (txt/md): no document *structure* to recover, so the
    # heavy docling stack is pointless — read the bytes, split paragraphs, emit
    # the same §3.1 envelope. Keeps plain ingestion working on deployments that
    # ship without the docling dependency (the all-in-one image installs it only
    # for the real-model pass, D-R3 #483).
    ext = os.path.splitext(filepath)[1].lower().lstrip(".")
    if ext in ("txt", "md", "markdown"):
        return _parse_plain_text(filepath, max_pages, lang_hint)

    try:
        from docling.document_converter import DocumentConverter
    except ImportError as e:
        # Dependency missing — surface as SUBPROCESS_FAILED so the Agent gets a
        # clear, non-retryable signal (and D3.5 knows to install docling).
        return _error(SUBPROCESS_FAILED,
                      "docling not installed: %s" % e,
                      structured_data={"code": "CX_ERR_SUBPROCESS_FAILED",
                                       "hint": "pip install -r requirements-parser.txt"})

    start = time.time()
    try:
        converter = DocumentConverter()
        result = converter.convert(filepath)
    except Exception as e:  # noqa: BLE001 — map known doc failures, re-raise rest
        text = str(e).lower()
        if "password" in text or "encrypt" in text:
            return _error(PASSWORD_PROTECTED, "Password-protected document",
                          structured_data={"code": "CX_ERR_PASSWORD_PROTECTED",
                                           "hint": "Provide the password or upload an unencrypted version"})
        if "corrupt" in text or "cannot open" in text or "damaged" in text:
            return _error(CORRUPTED_FILE, "Corrupted file: %s" % e,
                          structured_data={"code": "CX_ERR_CORRUPTED_FILE",
                                           "magic_bytes_expected": "",
                                           "magic_bytes_actual": ""})
        raise  # unexpected → crash (non-zero exit) → SUBPROCESS_CRASHED on C++ side

    # Group elements by page, then assemble pages[]/paragraphs[] (v1.0.1).
    doc = result.document
    pages_map = {}  # page_num -> {"paragraphs": [...], "char_count": int}
    for element in doc.iterate_items():
        # iterate_items() yields (item, level) in newer docling; tolerate both.
        item = element[0] if isinstance(element, tuple) else element
        prov = getattr(item, "prov", None)
        page_no = prov[0].page_no if prov else -1
        text = getattr(item, "text", "") or ""
        if not text:
            continue
        conf = 0.9
        if prov and hasattr(prov[0], "confidence") and prov[0].confidence is not None:
            conf = float(prov[0].confidence)
        para = {
            "text": text,
            "page": page_no,
            "section": _parent_section(item),
            "type": _classify_type(item),
            "confidence": conf,
            "language": lang_hint or "",
            "char_offset": -1,   # offsets are a downstream extension
            "char_length": len(text),
        }
        bucket = pages_map.setdefault(page_no, {"paragraphs": [], "char_count": 0})
        bucket["paragraphs"].append(para)
        bucket["char_count"] += len(text)

    pages = []
    for page_num in sorted(pages_map):
        if max_pages and 0 < max_pages < len(pages) + 1:
            break  # honor max_pages (truncate)
        paras = pages_map[page_num]["paragraphs"]
        avg = sum(p["confidence"] for p in paras) / len(paras) if paras else 0.0
        pages.append({
            "page_num": page_num,
            "page_text": "\n".join(p["text"] for p in paras),
            "page_metadata": {
                "page_num": page_num,
                "parser_used": "docling",
                "page_confidence": avg,
                "is_scan_page": False,
                "char_count": pages_map[page_num]["char_count"],
            },
            "paragraphs": paras,
        })

    filename = os.path.basename(filepath)
    try:
        size = os.path.getsize(filepath)
    except OSError:
        size = 0
    title = ""
    try:
        title = getattr(doc, "name", "") or ""
    except Exception:  # noqa: BLE001
        title = ""

    return {
        "status": OK if pages else EMPTY_DOCUMENT,
        "parser": "docling",
        "metadata": {
            "filename": filename,
            "doc_title": title,
            "mime_type": "",
            "file_size_bytes": size,
            "page_count": len(pages),
            "doc_language": lang_hint or "",
            "upload_timestamp": int(time.time() * 1000),
            "parse_time_ms": int((time.time() - start) * 1000),
        },
        "pages": pages,
        "failed_pages": [],
        "error_msg": "" if pages else "Document produced no text",
        "retryable": False,
        "category": "NONE",
        "retry_after_ms": 0,
        "structured_data": {} if pages else {"empty_reason": "all_pages_blank"},
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Docling parser bridge (F06)")
    parser.add_argument("--filepath", required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--max-pages", type=int, default=200)
    parser.add_argument("--language-hint", default="")
    parser.add_argument("--output-format", default="json")
    args = parser.parse_args(argv)

    if args.output_format != "json":
        _emit(_error(INVALID_OUTPUT,
                     "unsupported output format: %s" % args.output_format,
                     structured_data={"code": "CX_ERR_INVALID_OUTPUT",
                                      "parse_error_pos": -1}))
        return 0

    result = parse_document(args.filepath, args.timeout,
                            args.max_pages, args.language_hint)
    _emit(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
