"""Take a snapshot of the public Nintendo Life Switch catalog (requires lxml and curl)."""

from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal, InvalidOperation
import argparse
import gzip
import json
from pathlib import Path
import re
import subprocess

from lxml import html


BROWSE = "https://www.nintendolife.com/games/browse?system=nintendo-switch%2Cswitch-eshop&sort=rating"
GAME = re.compile(r"/games/(?:nintendo-switch|switch-eshop)/[a-zA-Z0-9_~.%+-]+")
IMAGE = re.compile(r"https://images\.nintendolife\.com/[a-f0-9]+/[^?#\s]+\.(?:jpg|webp|png)")


def read_page(number):
    url = BROWSE + (f"&page={number}" if number != 1 else "")
    data = subprocess.check_output([
        "curl", "--fail", "--silent", "--show-error", "--location", "--max-time", "25",
        "--retry", "2", "--retry-delay", "2", url,
    ])
    document = html.fromstring(data)
    rows = {}
    for card in document.xpath("//div[contains(concat(' ',normalize-space(@class),' '),' item-wrap ')]"):
        links = card.xpath("./div[contains(@class,'info')]/div/p[contains(@class,'heading')]/a")
        if not links:
            continue
        link = links[0]
        path = (link.get("href") or "").removeprefix("https://www.nintendolife.com")
        titles = link.xpath("./span[contains(concat(' ',normalize-space(@class),' '),' title ')]")
        if not GAME.fullmatch(path) or not titles:
            continue
        name = " ".join(titles[0].text_content().split())
        covers = card.xpath("./div[contains(@class,'cover')]//img")
        cover = covers[0].get("data-src") or covers[0].get("src", "") if covers else ""
        rating = 0
        score = card.xpath(".//p[contains(@class,'user-rating')]/span[@title]")
        if score:
            try:
                rating = int(Decimal(score[0].text_content().strip()) * 10)
            except (InvalidOperation, ValueError, OverflowError):
                pass
        if name:
            rows[path] = [name, cover if IMAGE.fullmatch(cover) else "", rating if 1 <= rating <= 100 else 0]
    more = bool(document.xpath("//nav[contains(@class,'ui-listing-pagination')]//a[@rel='next']"))
    if not rows:
        raise ValueError(f"Nintendo Life page {number} contained no Switch cards")
    return rows, more


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    catalog = {}
    with ThreadPoolExecutor(max_workers=4) as worker:
        first = 1
        more = True
        while more:
            for number, (rows, more) in enumerate(worker.map(read_page, range(first, first + 4)), first):
                catalog.update(rows)
                if number == 1 or number % 20 == 0 or not more:
                    print(f"Nintendo Life page {number}: {len(catalog)} Switch entries", flush=True)
                if not more:
                    break
            first = number + 1
    data = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    args.output.write_bytes(gzip.compress(data.encode(), mtime=0))
    print(f"Nintendo Life: {len(catalog)} games, {sum(bool(row[2]) for row in catalog.values())} user ratings")


if __name__ == "__main__":
    main()
