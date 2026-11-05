#!/usr/bin/env python3
"""Check every internal link and anchor in a built copy of the site.

Reads the build output directory (public/ by default) and checks:

- HTML pages: href and src attributes.
- The Markdown copies of pages (index.md) and llms.txt, which agents read:
  Markdown link targets and bare URLs.

A link is internal when it starts with the site's base path (/docs/), with
its full base URL (https://cortrix.ai/docs/), or is relative. Each internal
link must point to a file in the build output, and a #fragment must match an
id or name in the target page. A fragment on a Markdown copy is checked
against the HTML page it was made from. External links are not checked, so
the result never depends on the network.

Prints one line per broken link with the file it appears in, and exits 1 if
there are any.

Usage: check_links.py [--base-url URL] [public-dir]
"""

from __future__ import annotations

import argparse
import html.parser
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urljoin, urlsplit

DEFAULT_BASE_URL = "https://cortrix.ai/docs/"
SKIP_SCHEMES = ("mailto:", "tel:", "data:", "javascript:")

# Markdown link targets: [text](target) or ![alt](target "title").
MD_LINK = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
# Bare URLs, as in llms.txt.
BARE_URL = re.compile(r"https?://[^\s)<>\"'`]+")
# Code is example text, not navigation.
FENCE = re.compile(r"^(```|~~~).*?^\1[^\n]*$", re.M | re.S)
INLINE_CODE = re.compile(r"`[^`\n]*`")


class PageParser(html.parser.HTMLParser):
    """Collects link targets and anchor ids from one HTML page."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.links: list[str] = []
        self.ids: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for name, value in attrs:
            if value is None:
                continue
            if name == "id" or (tag == "a" and name == "name"):
                self.ids.add(value)
            if name in ("href", "src"):
                self.links.append(value)

    handle_startendtag = handle_starttag


class Site:
    def __init__(self, root: Path, base_url: str) -> None:
        self.root = root
        self.base_url = base_url if base_url.endswith("/") else base_url + "/"
        self.base_path = urlsplit(self.base_url).path
        self._pages: dict[Path, PageParser] = {}

    def page(self, path: Path) -> PageParser:
        if path not in self._pages:
            parser = PageParser()
            parser.feed(path.read_text(encoding="utf-8", errors="replace"))
            self._pages[path] = parser
        return self._pages[path]

    def url_of(self, path: Path) -> str:
        """The published URL of a file in the build output."""
        rel = path.relative_to(self.root).as_posix()
        if rel == "index.html":
            rel = ""
        elif rel.endswith("/index.html"):
            rel = rel[: -len("index.html")]
        return self.base_url + rel

    def resolve(self, link: str, source: Path) -> tuple[Path | None, str, str | None]:
        """Map a link to (target file, fragment, reason). Target None with no
        reason means the link is external or not a page link."""
        link = link.strip()
        if not link or link.startswith(SKIP_SCHEMES):
            return None, "", None
        url = urljoin(self.url_of(source), link)
        parts = urlsplit(url)
        full = f"{parts.scheme}://{parts.netloc}{parts.path}/"
        if not full.startswith(self.base_url):
            return None, "", None  # outside the site
        rel = unquote(parts.path[len(self.base_path) :])
        target = self.root / rel
        if rel == "" or rel.endswith("/"):
            target = target / "index.html"
        elif target.is_dir():
            target = target / "index.html"
        if not target.is_file():
            return None, parts.fragment, f"no file at {self.base_path}{rel}"
        return target, unquote(parts.fragment), None

    def anchor_page(self, target: Path) -> Path | None:
        """The HTML page whose ids a fragment on this target refers to."""
        if target.suffix == ".html":
            return target
        if target.name == "index.md":
            html_page = target.with_name("index.html")
            return html_page if html_page.is_file() else None
        return None


def links_in_text(text: str) -> list[str]:
    text = INLINE_CODE.sub("", FENCE.sub("", text))
    links = MD_LINK.findall(text)
    # Bare URLs that are not already the target of a Markdown link.
    stripped = MD_LINK.sub("", text)
    links += [u.rstrip(".,;:") for u in BARE_URL.findall(stripped)]
    return links


def check(root: Path, base_url: str) -> list[str]:
    site = Site(root, base_url)
    problems: list[str] = []
    sources = sorted(root.rglob("*.html")) + sorted(root.rglob("index.md"))
    if (root / "llms.txt").is_file():
        sources.append(root / "llms.txt")
    for source in sources:
        if source.suffix == ".html":
            links = site.page(source).links
        else:
            links = links_in_text(source.read_text(encoding="utf-8", errors="replace"))
        for link in links:
            if link.startswith("#"):
                target, fragment, reason = site.anchor_page(source), unquote(link[1:]), None
                if target is None:
                    continue
            else:
                target, fragment, reason = site.resolve(link, source)
            where = source.relative_to(root).as_posix()
            if reason:
                problems.append(f"{where}: {link} -> {reason}")
                continue
            if target is None or not fragment:
                continue
            anchor_page = site.anchor_page(target)
            if anchor_page is not None and fragment not in site.page(anchor_page).ids:
                shown = anchor_page.relative_to(root).as_posix()
                problems.append(f"{where}: {link} -> no id \"{fragment}\" in {shown}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("public_dir", nargs="?", default="public")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    args = parser.parse_args(argv)
    root = Path(args.public_dir).resolve()
    if not (root / "index.html").is_file():
        print(f"check_links.py: {root} has no index.html; build the site first", file=sys.stderr)
        return 2
    problems = check(root, args.base_url)
    for line in problems:
        print(line)
    if problems:
        print(f"check_links.py: {len(problems)} broken internal links", file=sys.stderr)
        return 1
    print("check_links.py: all internal links and anchors resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
