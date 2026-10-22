"""Download an official DuckDB CLI release for this machine, without installing globally."""
import argparse
import io
import json
import os
import pathlib
import platform
import tarfile
import urllib.request
import zipfile


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "duck-eviltransform-build"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Official release tag, e.g. v2.0.0")
    parser.add_argument("directory", type=pathlib.Path)
    args = parser.parse_args()
    system = {"Darwin": "osx", "Linux": "linux", "Windows": "windows"}[platform.system()]
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "amd64"
    release = json.loads(fetch(f"https://api.github.com/repos/duckdb/duckdb/releases/tags/{args.version}"))
    names = (f"duckdb-cli-{system}-{arch}.tar.gz", f"duckdb_cli-{system}-{arch}.zip")
    if system == "osx":
        names += ("duckdb_cli-osx-universal.zip",)
    assets = {item["name"]: item["browser_download_url"] for item in release["assets"]}
    name = next((name for name in names if name in assets), None)
    if name is None:
        raise RuntimeError(f"No official CLI found for {system}/{arch} in {args.version}")
    archive = io.BytesIO(fetch(assets[name]))
    executable = "duckdb.exe" if system == "windows" else "duckdb"
    if name.endswith(".zip"):
        with zipfile.ZipFile(archive) as source:
            member = next(x for x in source.namelist() if pathlib.PurePosixPath(x).name == executable)
            data = source.read(member)
    else:
        with tarfile.open(fileobj=archive, mode="r:gz") as source:
            member = next(x for x in source.getmembers() if pathlib.PurePosixPath(x.name).name == executable and x.isfile())
            data = source.extractfile(member).read()
    args.directory.mkdir(parents=True, exist_ok=True)
    target = args.directory / executable
    target.write_bytes(data)
    os.chmod(target, 0o755)
    print(target.resolve())


if __name__ == "__main__":
    main()
