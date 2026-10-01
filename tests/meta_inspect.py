#!/usr/bin/env python3
"""Small test-only reader for legacy and journaled myfs metadata."""

import struct
import sys


ENTRY_SIZE = 32


def _wire_entry(data, offset):
    logical, physical, raw, stored, checksum = struct.unpack_from(
        "<QQIII", data, offset
    )
    codec, flags = struct.unpack_from("<BB", data, offset + 28)
    return {
        "logical_offset": logical,
        "physical_offset": physical,
        "raw_size": raw,
        "stored_size": stored,
        "checksum": checksum,
        "codec_type": codec,
        "flags": flags,
    }


def load_meta(path):
    data = open(path, "rb").read()
    if data[:8] != b"MYFSMETA":
        count = struct.unpack_from("<I", data, 0)[0]
        logical_size = struct.unpack_from("<Q", data, 4)[0]
        chunks = []
        for i in range(count):
            logical, raw, stored, codec, flags, checksum, physical = (
                struct.unpack_from("<QIIBB2xIQ", data, 12 + ENTRY_SIZE * i)
            )
            chunks.append({
                "logical_offset": logical,
                "physical_offset": physical,
                "raw_size": raw,
                "stored_size": stored,
                "checksum": checksum,
                "codec_type": codec,
                "flags": flags,
            })
        return {
            "version": 0,
            "window_size": 65536,
            "logical_size": logical_size,
            "chunks": chunks,
        }

    version, header_size = struct.unpack_from("<HH", data, 8)
    window_size = 65536 if version == 1 else struct.unpack_from("<I", data, 20)[0]
    count = struct.unpack_from("<I", data, 24)[0]
    logical_size = struct.unpack_from("<Q", data, 32)[0]
    sequence = struct.unpack_from("<Q", data, 40)[0]
    chunks = [_wire_entry(data, header_size + ENTRY_SIZE * i)
              for i in range(count)]
    cursor = header_size + ENTRY_SIZE * count
    while cursor + 48 <= len(data) and data[cursor:cursor + 8] == b"MYFSDLTA":
        record_len = struct.unpack_from("<I", data, cursor + 8)[0]
        if cursor + record_len > len(data):
            break
        delta_sequence = struct.unpack_from("<Q", data, cursor + 16)[0]
        first, removed, added = struct.unpack_from("<III", data, cursor + 24)
        logical_size = struct.unpack_from("<Q", data, cursor + 40)[0]
        entries = [_wire_entry(data, cursor + 48 + ENTRY_SIZE * i)
                   for i in range(added)]
        chunks[first:first + removed] = entries
        sequence = delta_sequence
        cursor += record_len
    return {
        "version": version,
        "window_size": window_size,
        "logical_size": logical_size,
        "sequence": sequence,
        "chunks": chunks,
    }


def main():
    meta = load_meta(sys.argv[2])
    field = sys.argv[1]
    if field == "count":
        print(len(meta["chunks"]))
    elif field == "logical-size":
        print(meta["logical_size"])
    elif field == "codec0":
        print(meta["chunks"][0]["codec_type"])
    else:
        raise SystemExit("unknown field: " + field)


if __name__ == "__main__":
    main()
