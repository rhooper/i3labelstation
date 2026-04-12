#!/usr/bin/env python3
"""Generate src/card_db.h from cards.tsv.

TSV format: decimal_card_id<TAB>name (one per line).
Lines with empty/missing card IDs or #N/A are skipped.

Works both standalone and as a PlatformIO pre-build script.
"""

import os
import sys


def find_paths():
    """Return (tsv_path, out_path) based on execution context."""
    # Try __file__ first (standalone), fall back to CWD (PlatformIO)
    try:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_dir = os.path.dirname(script_dir)
    except NameError:
        project_dir = os.getcwd()

    repo_root = os.path.dirname(os.path.dirname(project_dir))
    return (
        os.path.join(repo_root, "cards.tsv"),
        os.path.join(project_dir, "src", "card_db.h"),
    )


def generate():
    tsv_path, out_path = find_paths()

    if not os.path.exists(tsv_path):
        print(f"WARNING: {tsv_path} not found, skipping card_db generation", file=sys.stderr)
        return

    entries = []
    with open(tsv_path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            parts = line.split("\t", 1)
            if len(parts) != 2:
                continue
            card_id_str, name = parts[0].strip(), parts[1].strip()
            if not card_id_str or card_id_str == "#N/A" or not name:
                continue
            try:
                card_id = int(card_id_str)
            except ValueError:
                print(f"WARNING: skipping line {lineno}: bad id '{card_id_str}'", file=sys.stderr)
                continue
            name_esc = name.replace("\\", "\\\\").replace('"', '\\"')
            entries.append((card_id, name_esc))

    content_lines = [
        "#pragma once",
        "",
        "// Auto-generated from cards.tsv — do not edit by hand.",
        f"// {len(entries)} entries",
        "",
        "typedef struct {",
        "    uint32_t card_id;",
        "    const char *name;",
        "} card_entry_t;",
        "",
        f"#define CARD_DB_COUNT {len(entries)}",
        "",
        "static const card_entry_t card_db[] = {",
    ]
    for card_id, name in entries:
        content_lines.append(f'    {{0x{card_id:08x}, "{name}"}},')
    content_lines.append("};")
    content_lines.append("")

    new_content = "\n".join(content_lines)

    # Only write if changed (avoid unnecessary rebuilds)
    if os.path.exists(out_path):
        with open(out_path, "r") as f:
            if f.read() == new_content:
                print(f"card_db.h unchanged ({len(entries)} entries)")
                return

    with open(out_path, "w") as f:
        f.write(new_content)

    print(f"Generated {out_path} with {len(entries)} entries")


# PlatformIO pre-build hook
try:
    Import("env")
    generate()
except Exception:
    pass

if __name__ == "__main__":
    generate()
