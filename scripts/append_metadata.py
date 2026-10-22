"""Append DuckDB's C_STRUCT metadata footer (extension-ci-tools format, MIT).

Format source: https://github.com/duckdb/extension-ci-tools/blob/main/scripts/append_extension_metadata.py
"""
import pathlib
import sys


def field(value):
    data = value.encode("ascii")
    if len(data) > 32:
        raise ValueError("DuckDB metadata field exceeds 32 bytes")
    return data.ljust(32, b"\0")


def main():
    library, output, platform, version = sys.argv[1:]
    # A valid Wasm custom section prefix is also accepted for native extensions.
    signature = b"\x00\x93\x04\x10duckdb_signature\x80\x04"
    metadata = b"".join(field(x) for x in ("", "", "", "C_STRUCT", version, "v1.2.0", platform, "4"))
    pathlib.Path(output).write_bytes(pathlib.Path(library).read_bytes() + signature + metadata + bytes(256))


if __name__ == "__main__":
    main()
