#!/usr/bin/env bash
# Drive Ghidra headless: import the IROM segment (main code) at its load addr,
# then run a preScript that maps every other segment, then a postScript that
# finds our target functions and dumps decompilation.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GHIDRA_SUPPORT="/opt/homebrew/Cellar/ghidra/12.1.2/libexec/support"
export JAVA_HOME="$(/opt/homebrew/bin/brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
export PATH="$JAVA_HOME/bin:$PATH"

PROJECT_DIR="$HERE/ghidra_project"
PROJECT_NAME="pandavent"
SEG_DIR="$HERE/segments"
OUT_DIR="$HERE/decomp"

mkdir -p "$PROJECT_DIR" "$OUT_DIR"

# Primary import: seg4 (IROM) — biggest code segment, contains most functions.
PRIMARY_SEG="seg4"
PRIMARY_BIN="$SEG_DIR/${PRIMARY_SEG}_400d0020.bin"
PRIMARY_BASE="0x400d0020"

export SEGMENT_DIR="$SEG_DIR"
export PRIMARY_SEG
export ANALYSIS_OUT="$HERE"

# Remove any existing project to keep runs idempotent.
rm -rf "$PROJECT_DIR"/*
mkdir -p "$PROJECT_DIR"

# Use PyGhidra so our .py pre/post scripts work under Ghidra 12+.
source "$HERE/venv/bin/activate"

"$GHIDRA_SUPPORT/analyzeHeadless" \
    "$PROJECT_DIR" "$PROJECT_NAME" \
    --python-cmd python \
    -import "$PRIMARY_BIN" \
    -processor Xtensa:LE:32:default \
    -loader BinaryLoader \
    -loader-baseAddr "$PRIMARY_BASE" \
    -preScript "$HERE/load_segments.py" \
    -postScript "$HERE/find_gpio_pins.py" \
    -scriptPath "$HERE" \
    2>&1 | tee "$HERE/ghidra.log"

echo
echo "decompilation output in: $OUT_DIR"
ls -1 "$OUT_DIR" 2>/dev/null || true
