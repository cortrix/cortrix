#!/usr/bin/env python3
"""PDF parser using PyMuPDF (fitz). Outputs JSON: {text, title, page_count, needs_ocr}"""
import sys, json, fitz

def parse_pdf(file_path):
    try:
        doc = fitz.open(file_path)
        full_text = "\f".join(page.get_text() for page in doc)
        metadata = doc.metadata or {}
        title = metadata.get("title", "") or __import__('os').path.splitext(__import__('os').path.basename(file_path))[0]
        result = {"text": full_text, "title": title, "page_count": len(doc), "needs_ocr": not full_text.strip()}
        doc.close()
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(parse_pdf(sys.argv[1]) if len(sys.argv) >= 2 else 1)
