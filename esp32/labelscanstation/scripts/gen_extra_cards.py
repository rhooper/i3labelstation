#!/usr/bin/env python3
"""Generate src/extra_cards.h from codes.csv.

CSV format: decimal_card_id,name (one per line).
Only rewrites if content changed.

Works both standalone and as a PlatformIO pre-build script.
"""

import os
import sys


def find_paths():
    try:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_dir = os.path.dirname(script_dir)
    except NameError:
        project_dir = os.getcwd()

    repo_root = os.path.dirname(os.path.dirname(project_dir))
    return (
        os.path.join(repo_root, "codes.csv"),
        os.path.join(project_dir, "src", "extra_cards.h"),
    )


def generate():
    csv_path, out_path = find_paths()

    if not os.path.exists(csv_path):
        print(f"WARNING: {csv_path} not found, skipping extra_cards generation", file=sys.stderr)
        # Write empty header so build doesn't break
        content = '#pragma once\n\n#define EXTRA_CARD_COUNT 0\n\nstatic const struct { uint32_t card_id; const char *name; } extra_cards[] = {};\n'
        if os.path.exists(out_path):
            with open(out_path, "r") as f:
                if f.read() == content:
                    return
        with open(out_path, "w") as f:
            f.write(content)
        return

    entries = []
    with open(csv_path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 1)
            if len(parts) != 2:
                continue
            card_id_str, name = parts[0].strip(), parts[1].strip()
            if not card_id_str or not name:
                continue
            try:
                card_id = int(card_id_str)
            except ValueError:
                print(f"WARNING: skipping line {lineno}: bad id '{card_id_str}'", file=sys.stderr)
                continue
            name_esc = name.replace("\\", "\\\\").replace('"', '\\"')
            entries.append((card_id, name_esc))

    lines = [
        "#pragma once",
        "",
        "// Auto-generated from codes.csv — do not edit by hand.",
        f"// {len(entries)} entries",
        "",
        f"#define EXTRA_CARD_COUNT {len(entries)}",
        "",
        "static const struct { uint32_t card_id; const char *name; } extra_cards[] = {",
    ]
    for card_id, name in entries:
        lines.append(f'    {{0x{card_id:08x}, "{name}"}},')
    lines.append("};")
    lines.append("")

    content = "\n".join(lines)

    if os.path.exists(out_path):
        with open(out_path, "r", encoding="utf-8") as f:
            if f.read() == content:
                print(f"extra_cards.h unchanged ({len(entries)} entries)")
                return

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"Generated {out_path} with {len(entries)} entries")


# PlatformIO pre-build hook
try:
    Import("env")
    generate()
except Exception:
    pass

if __name__ == "__main__":
    generate()
