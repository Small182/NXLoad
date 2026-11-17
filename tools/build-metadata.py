"""Build NXLoad's read-only, title-keyed index from the TitleDB US English snapshot."""

import argparse
from datetime import datetime
import html
import json
from pathlib import Path
import re
import struct
from urllib.request import urlopen
import zlib


TITLEDB_URL = (
    "https://raw.githubusercontent.com/blawar/titledb/"
    "d424690446eec6f4484209b9a1c067c7f7511398/US.en.json"
)
MAGIC = b"NXLTDB01"
MAX_SOURCE = 128 * 1024 * 1024
IGNORED = "™®©\u00a0–—’‘…"


def title_key(title):
    return "".join(
        c.lower() if c.isascii() else c
        for c in title
        if c not in IGNORED and (not c.isascii() or c.isalnum() or c in "+&")
    )


def field_bytes(value, capacity):
    return value.encode("utf-8")[: capacity - 1].decode("utf-8", "ignore").encode("utf-8")


def text(value):
    if not isinstance(value, str):
        return ""
    value = html.unescape(re.sub(r"</?(?:br|p|div|li|ul|b|i|strong|em)\b[^>]*>", " ", value, flags=re.I))
    return "\n".join(line for row in value.splitlines() if (line := " ".join(row.split())))


def release_date(value):
    try:
        return datetime.strptime(str(value), "%Y%m%d").date().isoformat()
    except ValueError:
        return ""


def build(database):
    entries = {}
    for item in database.values():
        if not isinstance(item, dict) or item.get("isDemo") is not False:
            continue
        title_id = item.get("id")
        if not isinstance(title_id, str) or not re.fullmatch(r"01[0-9a-fA-F]{11}000", title_id):
            continue
        name = text(item.get("name"))
        key = title_key(name).encode("utf-8")
        if not 0 < len(key) < 192:
            continue
        description = text(item.get("description")) or text(item.get("intro"))
        fields = [
            field_bytes(name, 192),
            field_bytes(text(item.get("publisher")), 192),
            field_bytes(release_date(item.get("releaseDate")), 16),
            field_bytes(description, 1536),
        ]
        if not fields[0]:
            continue
        record = b"\0".join(fields) + b"\0"
        entries.setdefault(key, {})[title_id.upper()] = record

    records = []
    for key, alternatives in sorted(entries.items()):
        if len(alternatives) != 1:
            continue
        body = zlib.compress(next(iter(alternatives.values())))
        records.append(key + b"\0" + struct.pack("<H", len(body)) + body)

    offset = len(MAGIC) + 4 + 4 * len(records)
    offsets = bytearray()
    for record in records:
        offsets.extend(struct.pack("<I", offset))
        offset += len(record)
    return MAGIC + struct.pack("<I", len(records)) + offsets + b"".join(records), len(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, help="Read a local TitleDB JSON snapshot")
    args = parser.parse_args()
    if args.source:
        data = args.source.read_bytes()
    else:
        with urlopen(TITLEDB_URL, timeout=60) as response:
            data = response.read(MAX_SOURCE + 1)
    if len(data) > MAX_SOURCE:
        raise ValueError("TitleDB snapshot exceeds the input limit")
    index, count = build(json.loads(data))
    if not count:
        raise ValueError("TitleDB snapshot contains no matching records")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".tmp")
    temporary.write_bytes(index)
    temporary.replace(args.output)
    print(f"TitleDB: {count} games, {len(index):,} bytes")


if __name__ == "__main__":
    main()
