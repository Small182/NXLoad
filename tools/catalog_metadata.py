"""Compile the lightweight title, cover and sorting index used by the library."""

from collections import defaultdict
from datetime import date
import gzip
import json
import re

from title_db import selected_games, text, title_key


# Bit order shared with the genre labels in source/main.c.
GENRES = (
    "Action", "Adventure", "Arcade", "Board Game", "Education", "Fighting",
    "First-Person Shooter", "Music", "Party", "Platformer", "Puzzle", "Racing",
    "RPG", "Shooter", "Simulation", "Sports", "Strategy", "Other",
)
GAME_PATH = re.compile(r"/games/(?:nintendo-switch|switch-eshop)/[a-zA-Z0-9_~.%+-]+")
COVER = re.compile(r"https://images\.nintendolife\.com/[a-f0-9]+/[^?#\s]+\.(?:jpg|webp|png)")
ESHOP_ICON = re.compile(r"https://img-eshop\.cdn\.nintendo\.net/i/[a-f0-9]{64}\.jpg")


def released_on(value):
    try:
        digits = f"{int(value):08d}"
        date(int(digits[:4]), int(digits[4:6]), int(digits[6:]))
        return int(digits)
    except (TypeError, ValueError):
        return 0


def cell(value, max_bytes):
    line = " ".join(value.split())
    return line.encode()[:max_bytes].decode("utf-8", "ignore")


def build_catalog_metadata(database, nintendo_life):
    rows = {}
    for encoded_key, _, name, item in selected_games(database):
        categories = item.get("category")
        mask = sum(1 << i for i, genre in enumerate(GENRES)
                   if isinstance(categories, list) and genre in categories)
        if mask & (1 << 6):
            mask |= 1 << 13
        # Nlib provides independently hosted HTTPS JPEG icons by canonical
        # Switch Title ID. Older eShop CDN icons may be blocked by DNS.mitm
        # on updated Switch firmware, leaving otherwise valid catalog games
        # with a blank cover. Prefer Nlib without changing screenshot sources.
        title_id = item.get("id", "")
        if isinstance(title_id, str) and re.fullmatch(r"[0-9a-fA-F]{16}", title_id):
            icon = f"https://api.nlib.cc/nx/{title_id.upper()}/icon/256"
        else:
            icon = item.get("iconUrl") or ""
            if not isinstance(icon, str) or not ESHOP_ICON.fullmatch(icon):
                icon = ""
            else:
                icon += "?w=256&h=256"
        rows[encoded_key.decode()] = (cell(name, 191), icon, "", "", released_on(item.get("releaseDate")), mask, 0)

    matches = defaultdict(list)
    for path, (name, cover, rating) in nintendo_life.items():
        name = cell(text(name), 191)
        key = title_key(name)
        if key and GAME_PATH.fullmatch(path) and len(path) < 480:
            cover = cover if COVER.fullmatch(cover) and len(cover) < 512 else ""
            matches[key].append((name, cover, path, rating if 1 <= rating <= 100 else 0))

    for key, candidates in matches.items():
        systems = {row[2].split("/")[2] for row in candidates}
        if len(systems) != len(candidates):
            continue
        name, cover, path, rating = max(
            candidates, key=lambda row: (bool(row[3]), bool(row[1]), row[2].startswith("/games/nintendo-switch/"))
        )
        if not cover:
            cover = next((row[1] for row in candidates if row[1]), "")
        _, icon, _, _, released, genres, _ = rows.get(key, ("", "", "", "", 0, 0, 0))
        rows[key] = (name, icon, cover, path, released, genres, rating)

    lines = ["NXLINFO2\n"]
    for key, row in sorted(rows.items()):
        lines.append("\t".join(map(str, (key, *row))) + "\n")
    return "".join(lines).encode(), sum(bool(row[3]) for row in rows.values())


def load_nintendo_life(path, ratings_path=None):
    catalog = json.loads(gzip.decompress(path.read_bytes()))
    if ratings_path:
        # Some detail pages display reader scores absent from the browse snapshot.
        for game, rating in json.loads(ratings_path.read_text(encoding="utf-8")).items():
            if not GAME_PATH.fullmatch(game) or type(rating) is not int or not 1 <= rating <= 100:
                raise ValueError(f"Invalid Nintendo Life reader score for {game}")
            if game in catalog and not catalog[game][2]:
                catalog[game][2] = rating
    return catalog
