"""Prepare, translate and combine the Ukrainian TitleDB description cache."""

import argparse
import gzip
import json
import os
from pathlib import Path
import re
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from title_db import description, fingerprint, load_catalog, selected_games


TOOLS = Path(__file__).parent
MANUAL = TOOLS / "descriptions.uk.json"
CACHE = TOOLS / "translations.uk.json.gz"
CYRILLIC = re.compile(r"[А-Яа-яІіЇїЄєҐґ]")
SENTENCE_END = re.compile(r"[.!?…][\"'’”»)]*$")
MAX_SEGMENT = 320


def read_json(path):
    data = path.read_bytes()
    return json.loads(gzip.decompress(data) if path.suffix == ".gz" else data)


def write_json(path, value):
    data = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_bytes(gzip.compress(data, mtime=0))
    temporary.replace(path)


def prepare(args):
    if not 1 <= args.shards <= 64:
        raise ValueError("The shard count must be between 1 and 64")
    manual = read_json(args.manual)
    saved = read_json(args.cache) if args.cache.is_file() else {}
    by_title, ready, pending = {}, {}, {}
    for _, title_id, _, item in selected_games(load_catalog(args.source)):
        english = description(item)
        if not english or manual.get(title_id, "").strip():
            continue
        digest = fingerprint(english)
        by_title[title_id] = digest
        cached = saved.get(title_id)
        if (isinstance(cached, list) and len(cached) == 2 and cached[0] == digest
                and isinstance(cached[1], str)):
            ready[digest] = cached[1]
        else:
            pending[digest] = english

    # Reuse one translation when multiple title IDs share the same description.
    for digest in ready:
        pending.pop(digest, None)
    queues = [{} for _ in range(args.shards)]
    lengths = [0 for _ in queues]
    for digest, english in sorted(pending.items(), key=lambda entry: (-len(entry[1]), entry[0])):
        index = min(range(args.shards), key=lengths.__getitem__)
        queues[index][digest] = english
        lengths[index] += len(english)
    for index, queue in enumerate(queues):
        write_json(args.output / f"input-{index}.json.gz", queue)
    write_json(args.output / "plan.json.gz", {"titles": by_title, "ready": ready})
    print(f"{len(by_title):,} games, {len(pending):,} distinct descriptions to translate, "
          f"{len(ready):,} reusable descriptions.", flush=True)
    print("pending=" + str(len(pending)), flush=True)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            print(f"pending={len(pending)}", file=output)


def token_segments(tokenizer, english):
    tokens = tokenizer.encode(english, out_type=str)
    while len(tokens) > MAX_SEGMENT:
        border = MAX_SEGMENT
        for i in range(MAX_SEGMENT, MAX_SEGMENT - 80, -1):
            if SENTENCE_END.search(tokens[i - 1]):
                border = i
                break
        yield tokens[:border]
        tokens = tokens[border:]
    if tokens:
        yield tokens


def run_translation(args):
    pending = read_json(args.input)
    if not pending:
        write_json(args.output, {})
        return

    import ctranslate2
    import sentencepiece

    with TemporaryDirectory(prefix="nxload-argos-") as temporary:
        with ZipFile(args.model) as archive:
            archive.extractall(temporary)
        model = Path(temporary) / "translate-en_uk-1_9"
        tokenizer = sentencepiece.SentencePieceProcessor(model_file=str(model / "sentencepiece.model"))
        translator = ctranslate2.Translator(
            str(model / "model"), compute_type="int8", inter_threads=2, intra_threads=2,
        )
        output = {}
        rows = list(pending.items())
        for start in range(0, len(rows), 48):
            batch = rows[start:start + 48]
            tokenized, ranges = [], []
            for digest, english in batch:
                first = len(tokenized)
                tokenized.extend(token_segments(tokenizer, english))
                ranges.append((digest, first, len(tokenized)))
            translated = translator.translate_batch(
                tokenized, beam_size=1, max_batch_size=2048, batch_type="tokens",
                max_input_length=MAX_SEGMENT, max_decoding_length=480, replace_unknowns=True,
            )
            for digest, first, last in ranges:
                parts = [
                    tokenizer.decode_pieces(result.hypotheses[0]).replace("▁", " ").strip()
                    for result in translated[first:last] if result.hypotheses
                ]
                ukrainian = " ".join(" ".join(parts).split())
                output[digest] = ukrainian if CYRILLIC.search(ukrainian) else ""
            if start % 192 == 0 or start + len(batch) == len(rows):
                print(f"Translated {start + len(batch):,} / {len(rows):,}", flush=True)
        write_json(args.output, output)


def merge(args):
    plan = read_json(args.plan)
    descriptions = plan["ready"]
    for part in sorted(args.parts.glob("output-*.json.gz")):
        for digest, translated in read_json(part).items():
            if digest in descriptions or not isinstance(translated, str):
                raise ValueError(f"Duplicate or invalid result for {digest}")
            descriptions[digest] = translated
    titles = plan["titles"]
    required = set(titles.values())
    if set(descriptions) != required:
        raise ValueError(f"{len(required - descriptions.keys())} missing and "
                         f"{len(descriptions.keys() - required)} unexpected descriptions")
    output = {title_id: [digest, descriptions[digest]] for title_id, digest in titles.items()}
    write_json(args.output, output)
    translated = sum(bool(entry[1]) for entry in output.values())
    print(f"{translated:,} Ukrainian descriptions, {len(output) - translated:,} English fallbacks.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("prepare")
    setup.add_argument("output", type=Path)
    setup.add_argument("--source", type=Path)
    setup.add_argument("--shards", type=int, default=20)
    setup.add_argument("--manual", type=Path, default=MANUAL)
    setup.add_argument("--cache", type=Path, default=CACHE)
    setup.set_defaults(run=prepare)

    worker = commands.add_parser("translate")
    worker.add_argument("input", type=Path)
    worker.add_argument("output", type=Path)
    worker.add_argument("--model", type=Path, required=True)
    worker.set_defaults(run=run_translation)

    collect = commands.add_parser("merge")
    collect.add_argument("plan", type=Path)
    collect.add_argument("parts", type=Path)
    collect.add_argument("output", type=Path)
    collect.set_defaults(run=merge)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
