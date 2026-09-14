#!/usr/bin/env python3
"""Validate Godot-style XML API references kept beside Trench Engine's source."""
from __future__ import annotations

import re
import sys
from pathlib import Path
from xml.etree import ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
API = ROOT / "docs" / "api"


def fail(message: str) -> None:
    print(f"API docs: {message}", file=sys.stderr)
    raise SystemExit(1)


def text(element: ET.Element | None) -> str:
    return "" if element is None or element.text is None else element.text.strip()


def header_symbols(path: Path) -> set[str]:
    source = path.read_text(encoding="utf-8")
    # Public function declarations in this C99 project end in a semicolon. This intentionally does
    # not attempt to parse C; XML references are a checked contract, not a header generator.
    return set(re.findall(r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*;", source, re.S))


def check(path: Path) -> int:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as error:
        fail(f"{path.relative_to(ROOT)} is not well-formed XML: {error}")
    if root.tag != "class" or not root.get("name") or not root.get("source"):
        fail(f"{path.relative_to(ROOT)} needs class name and source attributes")
    if not text(root.find("brief_description")) or not text(root.find("description")):
        fail(f"{path.relative_to(ROOT)} needs non-empty brief_description and description")
    header = ROOT / root.attrib["source"]
    if not header.is_file():
        fail(f"{path.relative_to(ROOT)} names missing header {root.attrib['source']}")
    available = header_symbols(header)
    names: set[str] = set()
    methods = root.findall("./methods/method")
    if not methods:
        fail(f"{path.relative_to(ROOT)} has no methods")
    for method in methods:
        name = method.get("name", "")
        if not name or name in names:
            fail(f"{path.relative_to(ROOT)} has a missing or duplicate method name")
        names.add(name)
        if name not in available:
            fail(f"{path.relative_to(ROOT)} documents {name}, absent from {header.relative_to(ROOT)}")
        if not text(method.find("description")):
            fail(f"{path.relative_to(ROOT)} method {name} has no description")
    print(f"API docs: PASS {path.relative_to(ROOT)} ({len(methods)} methods)")
    return len(methods)


def main() -> None:
    files = sorted(API.glob("**/*.xml"))
    if not files:
        fail("no XML references found")
    count = sum(check(path) for path in files)
    print(f"API docs: PASS {len(files)} references, {count} methods")


if __name__ == "__main__":
    main()
