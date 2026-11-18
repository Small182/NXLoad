"""Build NXLoad's read-only, title-keyed index from the TitleDB US English snapshot."""

import argparse
from datetime import datetime
from pathlib import Path
import re
import struct
import zlib

from catalog_metadata import build_catalog_metadata, load_nintendo_life
from title_db import description, load_catalog, selected_games, text

MAGIC = b"NXLTDB03"
SCREENSHOT = re.compile(r"https://img-eshop\.cdn\.nintendo\.net/i/([a-f0-9]{64})\.jpg")


def field_bytes(value, capacity):
    return value.encode("utf-8")[: capacity - 1].decode("utf-8", "ignore").encode("utf-8")


def release_date(value):
    try:
        return datetime.strptime(str(value), "%Y%m%d").date().isoformat()
    except ValueError:
        return ""


def build(database):
    records = []
    for key, _, name, item in selected_games(database):
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
            field_bytes(description(item), 6144),
            b"",  # Alternate description field reserved by the ROMFS index format.
            "".join(hashes).encode("ascii"),
        ]
        body = zlib.compress(b"\0".join(fields) + b"\0")
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
    parser.add_argument("--catalog-output", type=Path)
    parser.add_argument("--nintendo-life", type=Path)
    parser.add_argument("--nintendo-life-ratings", type=Path, help="Verified reader scores from individual game pages")
    args = parser.parse_args()
    database = load_catalog(args.source)
    index, count = build(database)
    if not count:
        raise ValueError("TitleDB snapshot contains no matching records")
    if args.catalog_output:
        nintendo_life = load_nintendo_life(args.nintendo_life, args.nintendo_life_ratings)
        data, matched = build_catalog_metadata(database, nintendo_life)
        write_index(args.catalog_output, data)
        print(f"Nintendo Life: {matched} unambiguous entries in the library metadata index")
    write_index(args.output, index)
    print(f"TitleDB: {count} games, English descriptions, {len(index):,} bytes")


def write_index(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


if __name__ == "__main__":
    main()
