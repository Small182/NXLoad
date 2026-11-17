"""Build NXLoad's read-only, title-keyed index from the TitleDB US English snapshot."""

import argparse
from datetime import datetime
import gzip
import json
from pathlib import Path
import re
import struct
import zlib


from title_db import description, fingerprint, load_catalog, selected_games, text

MAGIC = b"NXLTDB03"
AI_REVISION = "nx-context-v1"
SCREENSHOT = re.compile(r"https://img-eshop\.cdn\.nintendo\.net/i/([a-f0-9]{64})\.jpg")


CYRILLIC = re.compile(r"[А-Яа-яІіЇїЄєҐґ]")
LATIN = re.compile(r"[A-Za-z]")
WORDS = re.compile(r"[^\W\d_]+", re.UNICODE)
APOSTROPHE = re.compile(r"([А-Яа-яІіЇїЄєҐґ])\s*['’]\s+([А-Яа-яІіЇїЄєҐґ])")
HYPHEN = re.compile(r"(\d)-\s+(?=[А-Яа-яІіЇїЄєҐґ])")


def clean_machine_description(candidate, source):
    candidate = text(candidate)
    if not candidate or not CYRILLIC.search(candidate) or any(mark in candidate for mark in ("�", "▁", "<unk>")):
        return ""
    words = WORDS.findall(candidate)
    # Proper names are valid Latin text. Mixed scripts inside a single word are rarely intentional.
    originals = set(WORDS.findall(source.casefold()))
    mixed = [word for word in words if LATIN.search(word) and CYRILLIC.search(word)
             and word.casefold() not in originals]
    if mixed:
        return ""
    if len(words) >= 12 and sum(bool(LATIN.search(word)) for word in words) * 3 > len(words):
        return ""
    folded = [word.casefold() for word in words]
    for width in (1, 2, 3, 4):
        for start in range(len(folded) - 3 * width + 1):
            phrase = folded[start:start + width]
            if (width > 1 or len(phrase[0]) >= 3) and phrase * 3 == folded[start:start + 3 * width]:
                return ""
    candidate = APOSTROPHE.sub(r"\1’\2", candidate)
    return HYPHEN.sub(r"\1-", candidate)


def field_bytes(value, capacity):
    return value.encode("utf-8")[: capacity - 1].decode("utf-8", "ignore").encode("utf-8")


def release_date(value):
    try:
        return datetime.strptime(str(value), "%Y%m%d").date().isoformat()
    except ValueError:
        return ""


def build(database, manual, automatic, ai_translations=None):
    ai_translations = ai_translations or {}
    records = []
    translated = 0
    for key, title_id, name, item in selected_games(database):
        english = description(item)
        ukrainian = text(manual.get(title_id))
        if not ukrainian and english:
            ai_key = title_id + ":" + fingerprint(english) + ":" + AI_REVISION
            ukrainian = clean_machine_description(ai_translations.get(ai_key), english)
        if not ukrainian and english:
            ukrainian = clean_machine_description(automatic.get(fingerprint(english)), english)
        translated += bool(ukrainian)
        summary = ukrainian or english
        shots = item.get("screenshots")
        hashes = []
        if isinstance(shots, list):
            for url in shots:
                match = SCREENSHOT.fullmatch(url) if isinstance(url, str) else None
                if match:
                    hashes.append(match[1])
                if len(hashes) == 32:
                    break
        fields = [
            field_bytes(name, 192),
            field_bytes(text(item.get("publisher")), 192),
            field_bytes(release_date(item.get("releaseDate")), 16),
            field_bytes(summary, 3072),
            field_bytes(english if ukrainian and english != ukrainian else "", 3072),
            "".join(hashes).encode("ascii"),
        ]
        body = zlib.compress(b"\0".join(fields) + b"\0")
        records.append(key + b"\0" + struct.pack("<H", len(body)) + body)

    offset = len(MAGIC) + 4 + 4 * len(records)
    offsets = bytearray()
    for record in records:
        offsets.extend(struct.pack("<I", offset))
        offset += len(record)
    return MAGIC + struct.pack("<I", len(records)) + offsets + b"".join(records), len(records), translated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, help="Read a local TitleDB JSON snapshot")
    args = parser.parse_args()
    tools = Path(__file__).parent
    manual = json.loads((tools / "descriptions.uk.json").read_text(encoding="utf-8"))
    saved = tools / "translations.uk.json.gz"
    automatic = json.loads(gzip.decompress(saved.read_bytes())) if saved.is_file() else {}
    ai_saved = tools / "translations.ai.uk.json.gz"
    ai_translations = json.loads(gzip.decompress(ai_saved.read_bytes())) if ai_saved.is_file() else {}
    index, count, translated = build(load_catalog(args.source), manual, automatic, ai_translations)
    if not count:
        raise ValueError("TitleDB snapshot contains no matching records")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".tmp")
    temporary.write_bytes(index)
    temporary.replace(args.output)
    print(f"TitleDB: {count} games, {translated} Ukrainian descriptions, {len(index):,} bytes")


if __name__ == "__main__":
    main()
