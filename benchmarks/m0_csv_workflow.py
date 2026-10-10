"""Backend-independent M0-B1 CSV filter/transform using only the standard library.

Read ROOT/input.csv and exclusively create ROOT/output.csv. Both are ASCII
CSV files; output uses csv.writer's minimal quoting and LF line endings.
Input must have the exact INPUT_COLUMNS header and eight fields per record.
Quantity and price are nonempty unsigned ASCII decimal strings, including on
rejected rows. Blank records, embedded CR/LF/NUL fields, malformed quoting,
and non-ASCII input are rejected. Other fields are copied as strings.

Keep status == "ok" and int(quantity) >= 50, preserving order. total_cents
is the arbitrary-precision integer product of quantity and price_cents.
Files use 1 MiB buffering; no filesystem-specific code or durability mode
exists here. A nonzero exit means any partial output is UNVALIDATED. Success
means transformed, not independently validated; use m0_fixture.py validate.

transform(source, destination) accepts caller-owned, already-open text streams
and leaves them open. This lets a later wrapper define its own input lifecycle
without adding cache preparation, timing, or mount machinery here.

CLI: python3 -B benchmarks/m0_csv_workflow.py ROOT
Print a JSON transformation summary only after both files close successfully.
Exit 1 for malformed input/I/O errors and 2 for invalid command-line syntax.
"""

import argparse
import csv
import json
from pathlib import Path
import sys


BUFFER_BYTES = 1024 * 1024
INPUT_COLUMNS = (
    "event_id", "timestamp", "sensor_id", "region", "quantity",
    "price_cents", "status", "note",
)
OUTPUT_COLUMNS = ("event_id", "sensor_id", "region", "total_cents", "note")


def transform(source, destination):
    """Transform caller-owned text streams; raise on malformed input or I/O errors."""
    reader = csv.reader(source, strict=True)
    writer = csv.writer(destination, lineterminator="\n")
    input_rows = output_rows = 0
    try:
        if next(reader, None) != list(INPUT_COLUMNS):
            raise ValueError("missing or incorrect input header")
        writer.writerow(OUTPUT_COLUMNS)
        for row in reader:
            if len(row) != 8:
                raise ValueError(f"expected 8 fields at input line {reader.line_num}")
            if any(not value.isascii() or any(char in value for char in "\r\n\x00")
                   for value in row):
                raise ValueError(f"invalid ASCII field at input line {reader.line_num}")
            if not row[4].isdecimal() or not row[5].isdecimal():
                raise ValueError(f"invalid quantity or price_cents at input line {reader.line_num}")
            quantity, price_cents = int(row[4]), int(row[5])
            input_rows += 1
            if row[6] == "ok" and quantity >= 50:
                writer.writerow((row[0], row[2], row[3], quantity * price_cents, row[7]))
                output_rows += 1
    except csv.Error as error:
        raise ValueError(f"malformed CSV at input line {reader.line_num}: {error}") from error
    return {"input_rows": input_rows, "output_rows": output_rows}


def run(root):
    """Open ordinary paths with exclusive output creation; return after close."""
    root = Path(root)
    with (root / "input.csv").open("r", encoding="ascii", newline="", buffering=BUFFER_BYTES) as source:
        with (root / "output.csv").open("x", encoding="ascii", newline="", buffering=BUFFER_BYTES) as output:
            counts = transform(source, output)
    return dict(counts, status="transformed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    args = parser.parse_args(argv)
    try:
        result = run(args.root)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        print(f"error: {error}; any partial output is unvalidated", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
