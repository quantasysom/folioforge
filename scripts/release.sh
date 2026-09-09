#!/usr/bin/env bash
set -euo pipefail
script_path="${BASH_SOURCE[0]}"
script_parent="${script_path%/*}"
[[ "$script_parent" != "$script_path" ]] || script_parent=.
script_dir="$(cd -- "$script_parent" && pwd)"
args=()
while (($#)); do
  case "$1" in
    --help|-h)
      while IFS= read -r line; do printf '%s\n' "$line"; done <<'HELP'
Usage: bash scripts/release.sh [options]
  --qt-root PATH          Qt installation prefix (or QT_ROOT environment)
  --qpdf-root PATH        QPDF installation prefix (or QPDF_ROOT)
  --pdfium-root PATH      PDFium installation prefix (or PDFIUM_ROOT)
  --formats LIST         Comma-separated: Windows ZIP,MSI,EXE;
                         Linux TGZ,DEB,RPM; macOS DMG,PKG,ZIP,TGZ
  --build-dir PATH        Build directory; default build/release-<OS>
  --output-dir PATH       Package directory; default dist/<OS>
  --generator NAME       CMake generator; default Ninja
  --toolchain PATH        Optional CMake toolchain file
  --notices-dir PATH      Third-party notices directory to include
  --wix-version 3|4       WiX version for MSI; default 3
  --skip-tests           Explicitly skip the test suites
Build on the target OS. On Windows use Git Bash in an MSVC developer shell,
or use release.ps1, which can initialize MSVC automatically.
HELP
      exit 0 ;;
    --skip-tests) args+=("-DSKIP_TESTS=ON"); shift; continue ;;
    --qt-root) key=QT_ROOT ;;
    --qpdf-root) key=QPDF_ROOT ;;
    --pdfium-root) key=PDFIUM_ROOT ;;
    --formats) key=FORMATS ;;
    --build-dir) key=BUILD_DIR ;;
    --output-dir) key=OUTPUT_DIR ;;
    --generator) key=GENERATOR ;;
    --toolchain) key=TOOLCHAIN ;;
    --notices-dir) key=NOTICES_DIR ;;
    --wix-version) key=WIX_VERSION ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  if (($# < 2)) || [[ "$2" == --* ]]; then echo "Missing value for $1" >&2; exit 2; fi
  args+=("-D${key}=$2")
  shift 2
done
cmake "${args[@]}" -P "$script_dir/release.cmake"
