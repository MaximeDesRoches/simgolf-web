#!/usr/bin/env python3
"""Converts SimGolf golfer files (.chr, .pro, .glf) to and from JSON.

    tools/mods/chr.py decode "Themes/Firaxis/Kelley.chr" > kelley.json
    tools/mods/chr.py encode kelley.json "Kelley.chr"

The format is in docs/formats/golfers.md. The JSON names every field the
game reads; the quote table is keyed by event, and a portrait (a 140x420
8-bit PCX) is kept as a separate file next to the JSON ("portrait"). Bytes
the game ignores (padding after a name's NUL) aren't kept, so a round trip
gives an equivalent file rather than an identical one.
"""
import json
import os
import struct
import sys

PERSONALITY = ["neat", "outgoing", "active", "playful", "nice"]
AGES = {"young": 1, "middle aged": 2, "mature": 4}
MARITAL = {"single": 0x08, "married": 0x10, "divorced": 0x20, "widowed": 0x40}
PREFS = ["length", "accuracy", "imagination"]
# quote slots in file order (golfers.md 2.2); 20-24 are never used
QUOTES = ["greeting", "good shot", "easy shot", "hazard in the way", "striking feature", "bad result",
          "slot 6", "nearly hit", "hit an obstacle", "water", "likes a feature", "dislikes a feature",
          "animal", "slow play", "thirsty", "had a drink", "hungry", "ate", "tired", "sat on a bench"]
SKILLS = ["power hitter", "long driver", "accurate driver", "accurate irons", "accurate putter",
          "draw", "fade", "high backspin", "recovery", "luck"]
RECORD, QUOTE_LEN, SKILL_OFF, PCX_TAG = 0x230, 50, 0x712, 0x722


def cstr(b):
    return b.split(b"\0", 1)[0].decode("cp1252")


def pad(s, n):
    b = s.encode("cp1252")
    if len(b) >= n:
        raise SystemExit(f"{s!r} is longer than {n - 1} characters")
    return b + b"\0" * (n - len(b))


def one(table, value, what):
    if value not in table:
        raise SystemExit(f"{what} must be one of {', '.join(table)}, not {value!r}")
    return table[value]


def decode(path):
    data = open(path, "rb").read()
    if len(data) < PCX_TAG:
        raise SystemExit(f"{path}: {len(data)} bytes, a golfer file has at least {PCX_TAG}")
    pers, amg, face, hatbody, shirt, pants, outfit, skin, hair = data[0x20:0x29]
    flags = struct.unpack_from("<I", data, 0x2C)[0]
    out = {
        "profession": cstr(data[0:0x10]),
        "name": cstr(data[0x10:0x20]),
        "personality": [p for i, p in enumerate(PERSONALITY) if pers >> i & 1],
        "gender": "female" if amg & 0x80 else "male",
        "age": next((k for k, v in AGES.items() if amg & v), "middle aged"),
        "marital": next((k for k, v in MARITAL.items() if amg & v), "single"),
        "head": face if face < 0x14 else "portrait",
        "hat": hatbody & 15, "body": hatbody >> 4 & 3, "shirt": shirt, "pants": pants,
        "outfit": outfit, "skin": skin, "hair": hair,
        "prefers": [p for i, p in enumerate(PREFS) if flags >> i & 1],
        "child": bool(flags & 8),
        "custom appearance": bool(flags & 0x80),
        "quotes": {},
        "skills": dict(zip(SKILLS, data[SKILL_OFF:SKILL_OFF + 10])),
    }
    for i, name in enumerate(QUOTES):
        q = cstr(data[RECORD + i * QUOTE_LEN:RECORD + (i + 1) * QUOTE_LEN])
        if q:
            out["quotes"][name] = q
    if len(data) > PCX_TAG + 8:
        pcx = os.path.splitext(os.path.basename(path))[0] + ".pcx"
        open(pcx, "wb").write(data[PCX_TAG + 8:])
        out["portrait"] = pcx
    return out


def encode(j, base):
    rec = bytearray(RECORD)
    rec[0:0x10] = pad(j.get("profession", ""), 0x10)
    rec[0x10:0x20] = pad(j["name"], 0x10)
    rec[0x20] = sum(1 << PERSONALITY.index(p) for p in j.get("personality", []))
    rec[0x21] = (one(AGES, j.get("age", "middle aged"), "age") | one(MARITAL, j.get("marital", "single"), "marital")
                 | (0x80 if j.get("gender", "male") == "female" else 0))
    head = j.get("head", 0)
    rec[0x22] = 0x14 if head == "portrait" else head
    if not (head == "portrait" or 0 <= head < 0x14):
        raise SystemExit("head must be 0-19 or \"portrait\"")
    rec[0x23] = j.get("hat", 0) % 10 | (j.get("body", 0) & 3) << 4
    rec[0x24], rec[0x25], rec[0x26] = j.get("shirt", 0) % 10, j.get("pants", 0) % 10, j.get("outfit", 0)
    rec[0x27], rec[0x28] = j.get("skin", 0) & 3, j.get("hair", 0) % 5
    flags = sum(1 << PREFS.index(p) for p in j.get("prefers", []))
    flags |= 8 if j.get("child") else 0
    flags |= 0x80 if j.get("custom appearance", True) else 0
    struct.pack_into("<I", rec, 0x2C, flags)
    quotes = bytearray(25 * QUOTE_LEN)
    for name, text in j.get("quotes", {}).items():
        if name not in QUOTES:
            raise SystemExit(f"unknown quote {name!r}; known: {', '.join(QUOTES)}")
        i = QUOTES.index(name)
        quotes[i * QUOTE_LEN:(i + 1) * QUOTE_LEN] = pad(text, QUOTE_LEN)
    skills = bytearray(16)
    for name, v in j.get("skills", {}).items():
        skills[SKILLS.index(name)] = v
    out = bytes(rec) + bytes(quotes) + bytes(skills)
    if head == "portrait" or j.get("portrait"):
        pcx = open(os.path.join(base, j["portrait"]), "rb").read()
        if pcx[0] != 0x0A or pcx[2] != 1 or pcx[3] != 8:
            raise SystemExit(f"{j['portrait']}: the portrait must be an 8-bit RLE PCX (140x420)")
        out += b"*PCXFILE" + pcx
    return out


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "decode":
        print(json.dumps(decode(sys.argv[2]), indent=2, ensure_ascii=False))
    elif len(sys.argv) == 4 and sys.argv[1] == "encode":
        j = json.load(open(sys.argv[2], encoding="utf-8"))
        open(sys.argv[3], "wb").write(encode(j, os.path.dirname(os.path.abspath(sys.argv[2]))))
    else:
        raise SystemExit(__doc__)
