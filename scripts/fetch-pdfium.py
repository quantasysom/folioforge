#!/usr/bin/env python3
"""Download a prebuilt PDFium (bblanchon/pdfium-binaries) into third_party/pdfium.

Usage: python3 scripts/fetch-pdfium.py [--dest DIR] [--release TAG]
Then: export PDFIUM_ROOT=<dest>. Works on Windows, macOS and Linux (x64/arm64).
"""
import argparse, io, os, platform, sys, tarfile, urllib.request

def asset():
    system = platform.system().lower()
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    os_name = {"windows": "win", "darwin": "mac", "linux": "linux"}.get(system)
    if not os_name:
        sys.exit(f"Unsupported platform: {system}")
    return f"pdfium-{os_name}-{arch}.tgz"

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser()
    parser.add_argument("--dest", default=os.path.join(root, "third_party", "pdfium"))
    parser.add_argument("--release", default="latest", help="pdfium-binaries tag, e.g. chromium/7202")
    args = parser.parse_args()
    name = asset()
    base = "https://github.com/bblanchon/pdfium-binaries/releases"
    url = f"{base}/latest/download/{name}" if args.release == "latest" else f"{base}/download/{args.release}/{name}"
    print(f"Downloading {url}")
    with urllib.request.urlopen(url) as response:
        data = response.read()
    os.makedirs(args.dest, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        dest = os.path.realpath(args.dest)
        for member in archive.getmembers():
            target = os.path.realpath(os.path.join(dest, member.name))
            if not (target == dest or target.startswith(dest + os.sep)):
                sys.exit(f"Refusing unsafe archive path: {member.name}")
        archive.extractall(dest)
    print(f"PDFium extracted to {args.dest}\nSet PDFIUM_ROOT={args.dest}")

main()
