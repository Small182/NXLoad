"""Download the Nintendo Life galleries bundled with NXLoad."""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
from urllib.error import URLError
from urllib.request import Request, urlopen


CDN = "https://images.nintendolife.com/screenshots"
MAX_IMAGE = 2 * 1024 * 1024
GALLERIES = {
    "12switch": (80784, 80785, 80786, 80787, 80788, 80789, 80781, 80782, 80783),
    "mysimscozybundle": (
        152765, 152766, 152767, 152753, 152754, 152755, 152756, 152757, 152758, 152759,
        152760, 152761, 152762, 152763, 152764,
    ),
}


def download(item):
    destination, image = item
    if destination.exists():
        cached = destination.read_bytes()
        if cached.startswith(b"\xff\xd8") and cached.endswith(b"\xff\xd9"):
            return
    request = Request(f"{CDN}/{image}/large.jpg", headers={"User-Agent": "NXLoad/1.0.10"})
    for attempt in range(3):
        try:
            with urlopen(request, timeout=25) as response:
                data = response.read(MAX_IMAGE + 1)
            if len(data) > MAX_IMAGE or not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
                raise ValueError(f"Invalid Nintendo Life JPEG {image}")
            temporary = destination.with_suffix(".tmp")
            temporary.write_bytes(data)
            temporary.replace(destination)
            return
        except (URLError, TimeoutError):
            if attempt == 2:
                raise


def main():
    marker = Path(sys.argv[1])
    assets = []
    for key, ids in GALLERIES.items():
        folder = marker.parent / key
        folder.mkdir(parents=True, exist_ok=True)
        assets.extend((folder / f"{image}.jpg", image) for image in ids)
    with ThreadPoolExecutor(max_workers=4) as workers:
        list(workers.map(download, assets))
    for key, ids in GALLERIES.items():
        (marker.parent / key / "images.txt").write_text("".join(f"{image}\n" for image in ids), encoding="ascii")
    marker.touch()
    print(f"Nintendo Life: {len(assets)} bundled JPEG images")


if __name__ == "__main__":
    main()
