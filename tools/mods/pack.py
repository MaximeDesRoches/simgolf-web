#!/usr/bin/env python3
"""Builds a mod .zip from a mod source folder (docs/modding.md).

    tools/mods/pack.py mods/seaside-club build/mods/seaside-club.zip

The source folder holds mod.json and files/, laid out like the game folder.
On the way into the zip:
  - <name>.chr.json / .pro.json / .glf.json become golfer files (tools/mods/chr.py)
  - .txt and .dta files are written as the game reads them: Windows-1252 with
    CRLF line endings (sources can be UTF-8 with LF)
  - everything else is copied as is
"""
import io
import json
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chr as golfer  # noqa: E402


def main(src, out):
    info = json.load(open(os.path.join(src, "mod.json"), encoding="utf-8"))
    if not info.get("id") or not info.get("name"):
        raise SystemExit("mod.json needs an id and a name")
    files = os.path.join(src, "files")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    n = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mod.json", json.dumps(info, indent=2, ensure_ascii=False) + "\n")
        for dirpath, dirnames, names in os.walk(files):
            dirnames.sort()
            for name in sorted(names):
                if name.startswith("."):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, files).replace(os.sep, "/")
                low = name.lower()
                if low.endswith((".chr.json", ".pro.json", ".glf.json")):
                    data = golfer.encode(json.load(open(path, encoding="utf-8")), dirpath)
                    rel = rel[:-5]
                elif low.endswith((".txt", ".dta")):
                    text = open(path, encoding="utf-8").read().replace("\r\n", "\n")
                    data = text.replace("\n", "\r\n").encode("cp1252")
                else:
                    data = open(path, "rb").read()
                z.writestr("files/" + rel, data)
                n += 1
    print(f"{out}: {info['name']} {info.get('version', '')}, {n} files")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
