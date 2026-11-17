"""Read the pinned TitleDB catalog and select the games NXLoad indexes."""

from hashlib import sha256
import html
import json
import re
from urllib.request import urlopen


TITLEDB_URL = (
    "https://raw.githubusercontent.com/blawar/titledb/"
    "d424690446eec6f4484209b9a1c067c7f7511398/US.en.json"
)
MAX_SOURCE = 128 * 1024 * 1024
IGNORED = "™®©\u00a0–—’‘…"
TITLE_ID = re.compile(r"01[0-9a-fA-F]{11}000")
MARKUP = re.compile(r"</?(?:br|p|div|li|ul|b|i|strong|em)\b[^>]*>", re.I)


def load_catalog(path=None):
    if path is None:
        with urlopen(TITLEDB_URL, timeout=75) as response:
            data = response.read(MAX_SOURCE + 1)
    else:
        data = path.read_bytes()
    if len(data) > MAX_SOURCE:
        raise ValueError("TitleDB snapshot exceeds the input limit")
    return json.loads(data)


def title_key(title):
    return "".join(
        c.lower() if c.isascii() else c
        for c in title
        if c not in IGNORED and (not c.isascii() or c.isalnum() or c in "+&")
    )


def text(value):
    if not isinstance(value, str):
        return ""
    value = html.unescape(MARKUP.sub(" ", value)).replace("\0", "")
    return "\n".join(line for row in value.splitlines() if (line := " ".join(row.split())))


def description(item):
    return text(item.get("description")) or text(item.get("intro"))


def fingerprint(value):
    return sha256(value.encode("utf-8")).hexdigest()


def selected_games(database):
    entries = {}
    for item in database.values():
        if not isinstance(item, dict) or item.get("isDemo") is not False:
            continue
        title_id = item.get("id")
        if not isinstance(title_id, str) or not TITLE_ID.fullmatch(title_id):
            continue
        name = text(item.get("name"))
        key = title_key(name).encode("utf-8")
        if 0 < len(key) < 192:
            entries.setdefault(key, {})[title_id.upper()] = (name, item)
    for key, alternatives in sorted(entries.items()):
        if len(alternatives) == 1:
            title_id, (name, item) = next(iter(alternatives.items()))
            yield key, title_id, name, item
