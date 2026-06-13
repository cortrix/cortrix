#!/usr/bin/env python3
"""Word (.docx) parser using python-docx. Outputs JSON: {text, title, page_count}"""
import sys, json
from docx import Document

def parse_word(file_path):
    try:
        if not file_path.lower().endswith('.docx'):
            print(json.dumps({"error": "Only .docx format is supported"}), file=sys.stderr)
            return 1
        doc = Document(file_path)
        paragraphs = [para.text for para in doc.paragraphs if para.text.strip()]
        full_text = "\n".join(paragraphs)
        title = doc.core_properties.title or __import__('os').path.splitext(__import__('os').path.basename(file_path))[0]
        page_count = max(1, len(full_text) // 3000)
        result = {"text": full_text, "title": title, "page_count": page_count}
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except Exception as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(parse_word(sys.argv[1]) if len(sys.argv) >= 2 else 1)
