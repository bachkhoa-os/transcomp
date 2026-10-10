"""Streaming synthetic M0-B1 fixtures and an independent offline CSV oracle.

CSV construction: generator ``myfs-m0-csv-v1``, schema ``myfs-m0-input-v1``.
The ASCII header is INPUT_COLUMNS joined by commas followed by LF. Seed is
an unsigned 64-bit integer (default 20261010). For zero-based record i, d is
SHA256(ASCII("myfs-m0-csv-v1|seed={seed}|record={i}\\n")); integers use decimal
without leading zeros in this domain string. Data fields, in column order:

* event_id: i, zero-padded to 10 decimal bytes.
* timestamp: 1700000000 + i, zero-padded to 10 decimal bytes.
* sensor_id: big-endian d[0:2] modulo 1024, zero-padded to 4 bytes.
* region: REGIONS[d[2] modulo 4], exactly 5 bytes.
* quantity: d[3] modulo 100, zero-padded to 2 bytes.
* price_cents: big-endian d[4:7] modulo 1000000, zero-padded to 6 bytes.
* status: "er" if d[7] modulo 5 is zero, otherwise "ok" (2 bytes).
* note: "batch={i // 4096:06d};topic={TOPICS[d[8] % 16]:12s};"
  "tag={d[9:17].hex()};source=synthetic;", right-padded with underscores to
  81 bytes. The topic is left-aligned and space-padded to 12 bytes.

Widths total 120 bytes; seven commas and the final LF make each record
exactly 128 ASCII bytes. No field contains commas, CR, or LF. Records are
limited to 8300000000 so the timestamp still fits its fixed width. Smoke is
32768 records; full is 1048576 records (128 MiB plus the header).

Binary construction: ``myfs-m0-shake256-v1``. Block j is the first 65536 bytes
of SHAKE256(ASCII("myfs-m0-shake256-v1|seed={seed}|block={j}\\n")), with the
same decimal encoding. Concatenate blocks starting at j=0 and take exactly
the requested length. The final block may be a prefix. This is a synthetic
high-entropy candidate, NOT empirically established incompressible data.

Files/manifests are exclusively created; a manifest is written only after
its data file closes successfully. Failed generation may leave incomplete
files, which are not validated fixtures. No durability guarantee is made.

The oracle below never imports the application. It parses input separately
with DictReader and builds canonical expected bytes with its own escaping.
It does not depend on the generator when validating hand-authored input.
Validation compares the complete byte streams and reports the first byte
mismatch, hashes, lengths, and completed LF-terminated data-row counts.
Actual row counts on corrupt output are descriptive, not proof of validity.

CLI (all standard-library, offline operations):
  python3 -B benchmarks/m0_fixture.py csv INPUT --records N [--seed S]
  python3 -B benchmarks/m0_fixture.py csv INPUT --size smoke|full
  python3 -B benchmarks/m0_fixture.py binary CONTROL --bytes N [--seed S]
  python3 -B benchmarks/m0_fixture.py binary CONTROL --size smoke|full
  python3 -B benchmarks/m0_fixture.py expected INPUT EXPECTED
  python3 -B benchmarks/m0_fixture.py validate INPUT OUTPUT

Generation defaults to INPUT.manifest.json / CONTROL.manifest.json; override
with --manifest PATH. Commands print JSON. Validation exits 0 only for exact
agreement, 1 for mismatch/errors; invalid CLI syntax exits 2. Expected-file
generation also refuses overwrite. No app execution or measurement occurs.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import sys


DEFAULT_SEED = 20261010
BUFFER_BYTES = 1024 * 1024
BLOCK_BYTES = 65536
CSV_GENERATOR = "myfs-m0-csv-v1"
BINARY_GENERATOR = "myfs-m0-shake256-v1"
INPUT_COLUMNS = (
    "event_id", "timestamp", "sensor_id", "region", "quantity",
    "price_cents", "status", "note",
)
OUTPUT_COLUMNS = ("event_id", "sensor_id", "region", "total_cents", "note")
REGIONS = ("north", "south", "east_", "west_")
TOPICS = (
    "ingest", "export", "sample", "parse", "filter", "aggregate",
    "validate", "archive", "sensor", "batch", "reading", "stream",
    "event", "pipeline", "measure", "record",
)


def _check_seed(seed):
    if not isinstance(seed, int) or isinstance(seed, bool) or not 0 <= seed < 2**64:
        raise ValueError("seed must be an unsigned 64-bit integer")


def _check_count(value, name, maximum=None):
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"{name} must be a nonnegative integer")
    if maximum is not None and value > maximum:
        raise ValueError(f"{name} must not exceed {maximum}")


def _csv_records(records, seed):
    yield (",".join(INPUT_COLUMNS) + "\n").encode("ascii")
    for index in range(records):
        domain = f"{CSV_GENERATOR}|seed={seed}|record={index}\n".encode("ascii")
        digest = hashlib.sha256(domain).digest()
        note = (
            f"batch={index // 4096:06d};topic={TOPICS[digest[8] % 16]:12s};"
            f"tag={digest[9:17].hex()};source=synthetic;"
        ).ljust(81, "_")
        fields = (
            f"{index:010d}", f"{1700000000 + index:010d}",
            f"{int.from_bytes(digest[:2], 'big') % 1024:04d}",
            REGIONS[digest[2] % 4], f"{digest[3] % 100:02d}",
            f"{int.from_bytes(digest[4:7], 'big') % 1000000:06d}",
            "er" if digest[7] % 5 == 0 else "ok", note,
        )
        record = (",".join(fields) + "\n").encode("ascii")
        if len(record) != 128:
            raise ValueError(f"record {index} violates the 128-byte construction")
        yield record


def _binary_blocks(length, seed):
    remaining, index = length, 0
    while remaining:
        domain = f"{BINARY_GENERATOR}|seed={seed}|block={index}\n".encode("ascii")
        size = min(BLOCK_BYTES, remaining)
        yield hashlib.shake_256(domain).digest(size)
        remaining -= size
        index += 1


def _write_fixture(data_path, manifest_path, chunks, metadata):
    data_path, manifest_path = Path(data_path), Path(manifest_path)
    if data_path.resolve() == manifest_path.resolve():
        raise ValueError("data and manifest paths must be distinct")
    # lexists also rejects dangling symlinks. Exclusive opens below protect
    # against a path appearing after this preflight inspection.
    for path in (data_path, manifest_path):
        if os.path.lexists(path):
            raise FileExistsError(f"refusing to overwrite {path}")
    digest, byte_count = hashlib.sha256(), 0
    with data_path.open("xb", buffering=BUFFER_BYTES) as data:
        with manifest_path.open("x", encoding="ascii", newline="\n") as manifest:
            for chunk in chunks:
                data.write(chunk)
                digest.update(chunk)
                byte_count += len(chunk)
            data.close()  # Surface flush/close errors before publishing metadata.
            result = dict(metadata, manifest_version="myfs-m0-manifest-v1",
                          seed=metadata["seed"], byte_count=byte_count,
                          sha256=digest.hexdigest())
            json.dump(result, manifest, sort_keys=True, indent=2, ensure_ascii=True)
            manifest.write("\n")
    return result


def generate_csv(data_path, manifest_path, records, seed=DEFAULT_SEED):
    """Exclusively create a streaming CSV fixture and return its manifest."""
    _check_seed(seed)
    _check_count(records, "record count", maximum=8300000000)
    metadata = {
        "kind": "synthetic_csv", "generator_version": CSV_GENERATOR,
        "schema_version": "myfs-m0-input-v1", "seed": seed,
        "record_count": records, "record_bytes": 128,
        "columns": list(INPUT_COLUMNS), "encoding": "ascii", "line_ending": "LF",
        "field_widths": [10, 10, 4, 5, 2, 6, 2, 81],
        "derivation": "SHA256(ASCII(myfs-m0-csv-v1|seed={seed}|record={i}\\n))",
    }
    return _write_fixture(data_path, manifest_path, _csv_records(records, seed), metadata)


def generate_binary(data_path, manifest_path, length, seed=DEFAULT_SEED):
    """Create a synthetic high-entropy candidate; do not assert compressibility."""
    _check_seed(seed)
    _check_count(length, "binary length")
    metadata = {
        "kind": "synthetic_high_entropy_candidate",
        "generator_version": BINARY_GENERATOR, "seed": seed,
        "requested_bytes": length, "block_bytes": BLOCK_BYTES,
        "construction": (
            "concatenate SHAKE256(ASCII(myfs-m0-shake256-v1|seed={seed}|block={j}\\n))"
            ".digest(65536) for j=0,1,...; truncate to requested_bytes"
        ),
        "compression_behavior": "not measured; not empirically incompressible",
    }
    return _write_fixture(data_path, manifest_path, _binary_blocks(length, seed), metadata)


def _oracle_input_lines(source):
    for number, line in enumerate(source, 1):
        if not line.isascii() or "\x00" in line or line in ("\n", "\r", "\r\n"):
            raise ValueError(f"oracle: invalid ASCII CSV input at physical line {number}")
        yield line


def _oracle_escape(value):
    if '"' in value or "," in value:
        return '"' + value.replace('"', '""') + '"'
    return value


def expected_lines(source):
    """Yield canonical expected bytes from a caller-owned ASCII text stream.

    DictReader parsing, named-column selection, and manual CSV escaping are
    independent of the application's reader/writer transformation. Quantities
    and prices must be nonempty unsigned ASCII decimal strings, even on rows
    that will not match. Embedded CR/LF/NUL fields and blank rows are invalid.
    """
    reader = csv.DictReader(_oracle_input_lines(source), strict=True)
    try:
        if reader.fieldnames != list(INPUT_COLUMNS):
            raise ValueError("oracle: missing or incorrect input header")
        yield (",".join(OUTPUT_COLUMNS) + "\n").encode("ascii")
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"oracle: wrong field count at line {reader.line_num}")
            if any(not value.isascii() or any(char in value for char in "\r\n\x00")
                   for value in row.values()):
                raise ValueError(f"oracle: invalid field at line {reader.line_num}")
            for key in ("quantity", "price_cents"):
                if not row[key].isascii() or not row[key].isdecimal():
                    raise ValueError(f"oracle: invalid {key} at line {reader.line_num}")
            quantity, price = int(row["quantity"]), int(row["price_cents"])
            if row["status"] != "ok" or quantity < 50:
                continue
            values = [row["event_id"], row["sensor_id"], row["region"],
                      str(quantity * price), row["note"]]
            yield (",".join(_oracle_escape(value) for value in values) + "\n").encode("ascii")
    except csv.Error as error:
        raise ValueError(f"oracle: malformed CSV at line {reader.line_num}: {error}") from error


class _ByteSummary:
    def __init__(self):
        self.digest = hashlib.sha256()
        self.byte_count = 0
        self.line_count = 0

    def add(self, chunk):
        self.digest.update(chunk)
        self.byte_count += len(chunk)
        self.line_count += chunk.count(b"\n")

    def result(self):
        return {"sha256": self.digest.hexdigest(), "byte_count": self.byte_count,
                "row_count": max(0, self.line_count - 1)}


def write_expected(input_path, output_path):
    """Exclusively create expected bytes; failures leave unvalidated partial data."""
    summary = _ByteSummary()
    with Path(input_path).open("r", encoding="ascii", newline="", buffering=BUFFER_BYTES) as source:
        with Path(output_path).open("xb", buffering=BUFFER_BYTES) as output:
            for line in expected_lines(source):
                output.write(line)
                summary.add(line)
    return summary.result()


def validate_output(input_path, output_path):
    """Compare every output byte with an independently generated expectation.

    first_mismatch has a zero-based byte offset and up to 16 expected/actual
    bytes in hex; an empty hex string denotes EOF on that side. Actual hashing
    continues after a mismatch, including unexpected trailing output.
    """
    expected, actual = _ByteSummary(), _ByteSummary()
    mismatch = None
    with Path(input_path).open("r", encoding="ascii", newline="", buffering=BUFFER_BYTES) as source:
        with Path(output_path).open("rb", buffering=BUFFER_BYTES) as output:
            for want in expected_lines(source):
                offset = expected.byte_count
                got = output.read(len(want))
                expected.add(want)
                actual.add(got)
                if mismatch is None and want != got:
                    index = next((i for i, pair in enumerate(zip(want, got))
                                  if pair[0] != pair[1]), min(len(want), len(got)))
                    mismatch = {"offset": offset + index,
                                "expected_hex": want[index:index + 16].hex(),
                                "actual_hex": got[index:index + 16].hex()}
            while chunk := output.read(BUFFER_BYTES):
                if mismatch is None:
                    mismatch = {"offset": expected.byte_count,
                                "expected_hex": "", "actual_hex": chunk[:16].hex()}
                actual.add(chunk)
    return {"oracle_version": "myfs-m0-oracle-v1", "passed": mismatch is None,
            "expected": expected.result(), "actual": actual.result(),
            "first_mismatch": mismatch}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, count_flag, count_dest in (("csv", "--records", "records"),
                                        ("binary", "--bytes", "length")):
        command = commands.add_parser(name)
        command.add_argument("path", type=Path)
        command.add_argument("--manifest", type=Path)
        command.add_argument("--seed", type=int, default=DEFAULT_SEED)
        size = command.add_mutually_exclusive_group(required=True)
        size.add_argument(count_flag, dest=count_dest, type=int)
        size.add_argument("--size", choices=("smoke", "full"))
    for name in ("expected", "validate"):
        command = commands.add_parser(name)
        command.add_argument("input", type=Path)
        command.add_argument("output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command in ("csv", "binary"):
            manifest = args.manifest if args.manifest is not None else Path(str(args.path) + ".manifest.json")
            if args.command == "csv":
                records = {"smoke": 32768, "full": 1048576}[args.size] if args.size else args.records
                result = generate_csv(args.path, manifest, records, args.seed)
            else:
                length = {"smoke": 4 * 1024**2, "full": 64 * 1024**2}[args.size] if args.size else args.length
                result = generate_binary(args.path, manifest, length, args.seed)
        elif args.command == "expected":
            result = write_expected(args.input, args.output)
        else:
            result = validate_output(args.input, args.output)
        print(json.dumps(result, sort_keys=True))
        return 0 if result.get("passed", True) else 1
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        print(json.dumps({"passed": False, "error": str(error)}, sort_keys=True))
        print(f"error: {error}; incomplete files are unvalidated", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
