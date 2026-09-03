#!/bin/bash
# Build savesync.nds.
#
# In the "Wonderful Toolchain Shell" launched from the Start menu, BLOCKSDS
# and PATH are already set, so a plain `make` works and you don't need this
# script. It exists for shells that don't have that setup (plain Git Bash,
# an IDE terminal, CI), where the toolchain has to be pointed at explicitly.
set -e

WONDERFUL=/c/msys64/opt/wonderful
export PATH="$WONDERFUL/bin:$WONDERFUL/toolchain/gcc-arm-none-eabi/bin:/c/msys64/usr/bin:$PATH"

cd "$(dirname "$0")"

# arm-none-eabi-gcc writes temporary files to the directory named by TMP/TEMP.
# When those don't reach it, it falls back to C:\WINDOWS and fails with a
# permission error, so pass them explicitly through env rather than export.
env "TMP=${TMP:-C:\\Users\\$USERNAME\\AppData\\Local\\Temp}" \
    "TEMP=${TEMP:-C:\\Users\\$USERNAME\\AppData\\Local\\Temp}" \
    make BLOCKSDS="$WONDERFUL/thirdparty/blocksds/core" "$@"
