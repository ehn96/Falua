#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Refreshes third_party/jiant from the JIANT repository (or another fork of Felucca) and regenerates
# the DSP tables. Needs git and python3.
#   tools/update_jiant.sh [repo-url] [branch]
set -e
URL=${1:-https://github.com/juanjiant-bit/JIANT.git}
BRANCH=${2:-main}
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
git clone --depth 1 --branch "$BRANCH" "$URL" "$TMP/src"
rm -rf third_party/jiant/firmware third_party/jiant/LICENSES
mkdir -p third_party/jiant/firmware third_party/jiant/generated
cp -R "$TMP/src/firmware/src" "$TMP/src/firmware/hal" third_party/jiant/firmware/
cp -R "$TMP/src/LICENSE" "$TMP/src/LICENSING.md" "$TMP/src/LICENSES" third_party/jiant/
python3 "$TMP/src/tools/gen_tables.py" third_party/jiant/generated/felucca_tables.h
python3 "$TMP/src/tools/gen_fm6_patches.py" third_party/jiant/generated/felucca_fm6.h
(cd "$TMP/src" && echo "$URL $(git log -1 --format='%H %cd')") > third_party/jiant/UPSTREAM_COMMIT.txt
rm -rf "$TMP"
echo "updated: $(cat third_party/jiant/UPSTREAM_COMMIT.txt)"
echo "now build and run core_test: every engine and preset must still sound"
