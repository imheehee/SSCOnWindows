#!/usr/bin/env python3
"""Inspect an A760 candidate without opening a device or modifying its files."""

import argparse
import hashlib
import json
import struct
from pathlib import Path

# Reported in the ipTIME support thread for the image in driver 20.11.3038.3001.
OEM_SHA256 = "f0437ab72b37d3ac4e3b6e9e5c22c74a0f5d21d9d276b728d7ed5461311a0ce2"
EXTENSION_SIGNATURE = bytes.fromhex("51 04 fd 77")
MAX_INPUT = 4 * 1024 * 1024


def read_candidate(path):
    with path.open("rb") as source:
        data = source.read(MAX_INPUT + 1)
    if len(data) > MAX_INPUT:
        raise ValueError(f"{path}: input exceeds {MAX_INPUT} bytes")
    return data


def inspect_epatch(data, rom):
    if len(data) < 26 or data[:8] != b"Realtech" or data[-4:] != EXTENSION_SIGNATURE:
        raise ValueError("expected an uncompressed Realtech legacy epatch with extension signature")

    version, count = struct.unpack_from("<IH", data, 8)
    table_end = 14 + 8 * count
    trailer = len(data) - 4
    if not count or table_end + 3 > trailer:
        raise ValueError("epatch table is truncated or empty")

    patches = {}
    for i in range(count):
        chip, = struct.unpack_from("<H", data, 14 + 2 * i)
        length, = struct.unpack_from("<H", data, 14 + 2 * count + 2 * i)
        offset, = struct.unpack_from("<I", data, 14 + 4 * count + 4 * i)
        if chip in patches or length < 4 or offset < table_end or offset + length > trailer:
            raise ValueError("duplicate chip ID or invalid epatch payload bounds")
        patches[chip] = (offset, length)

    payload_end = max(offset + length for offset, length in patches.values())
    project = None
    while trailer > payload_end:
        opcode = data[trailer - 1]
        if opcode == 0xff:
            break
        if trailer - payload_end < 2:
            raise ValueError("incomplete epatch extension record")
        length = data[trailer - 2]
        start = trailer - 2 - length
        if not length or start < payload_end:
            raise ValueError("invalid epatch extension length")
        if opcode == 0 and length == 1:
            project = data[start]
            break
        trailer = start

    if project is None:
        raise ValueError("epatch project ID not found")
    chip = rom + 1
    sha256 = hashlib.sha256(data).hexdigest()
    return {
        "sha256": sha256,
        "bytes": len(data),
        "reported_oem_hash_match": sha256 == OEM_SHA256,
        "epatch_version": f"0x{version:08X}",
        "project_id": project,
        "rom": rom,
        "chip_id": chip,
        "patch_bytes": patches[chip][1] if chip in patches else 0,
    }


def inspect_config(data):
    if len(data) < 6:
        raise ValueError("truncated configuration header")
    magic, size = struct.unpack_from("<IH", data)
    if magic != 0x8723AB55 or size != len(data) - 6:
        raise ValueError("incorrect Realtek configuration magic or size")
    entries = []
    offset = 6
    while offset < len(data):
        if len(data) - offset < 3:
            raise ValueError("truncated configuration entry")
        address, length = struct.unpack_from("<HB", data, offset)
        offset += 3 + length
        if offset > len(data):
            raise ValueError("truncated configuration entry payload")
        entries.append({"offset": f"0x{address:04X}", "bytes": length})
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "entries": entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=lambda value: int(value, 0), required=True,
                        help="value reported by HCI 0xFC6D (e.g. 0x01)")
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.rom <= 255:
        parser.error("--rom must fit in one byte")
    try:
        firmware = inspect_epatch(read_candidate(args.firmware), args.rom)
        config = inspect_config(read_candidate(args.config))
    except (OSError, ValueError) as error:
        parser.exit(2, f"A760 firmware inspection: {error}\n")

    print(json.dumps({
        "firmware": firmware,
        "config": config,
        "rtl8761bu_rom_candidate": args.rom == 1 and firmware["project_id"] == 14
                                 and firmware["patch_bytes"] > 0,
        "windows_winusb_load_approved": False,
    }, indent=2))


if __name__ == "__main__":
    main()
