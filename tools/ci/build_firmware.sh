#!/usr/bin/env bash
# Build firmware_v2/master once for a given label, with an incremental
# version, then package flashable artifacts into dist/.
#
# The version embedded in the compiled app (esp_app_desc_t.version, and the
# Trafyn latestVersion each artifact is published under) is:
#   <base-version-from-VERSION-file>-<label>.<BITBUCKET_BUILD_NUMBER>
# e.g. VERSION="1.0.33" + label=dev + build #42 -> "1.0.33-dev.42"
#
# BITBUCKET_BUILD_NUMBER increments on every pipeline run in this repo, so
# a given label's builds are always increasing, even though the counter is
# shared across all pipelines in the repo.
#
# Called with label=dev for the independent dev build, and label=rel for
# the staging/prod release-candidate build — staging and prod deploy the
# exact same compiled dist/ output from that one "rel" build rather than
# each compiling their own (see bitbucket-pipelines.yml).
#
# Usage: build_firmware.sh <label>   (label: dev | rel)
set -euo pipefail

ENV_NAME="${1:?usage: build_firmware.sh <env>}"
BUILD_NUM="${BITBUCKET_BUILD_NUMBER:?BITBUCKET_BUILD_NUMBER not set}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP_DIR="${ROOT}/firmware_v2/master"
VERSION_FILE="${APP_DIR}/VERSION"

cd "$APP_DIR"

BASE_VERSION="$(cut -d'-' -f1 "$VERSION_FILE" | tr -d '[:space:]')"
FW_VERSION="${BASE_VERSION}-${ENV_NAME}.${BUILD_NUM}"
echo "Building ${FW_VERSION} for ${ENV_NAME} in ${APP_DIR}"

# Overwrite the working copy only (never committed) so this exact string is
# baked into the app image via CMakeLists.txt's PROJECT_VER.
echo "$FW_VERSION" > "$VERSION_FILE"

# Pulls cmake/ninja/esptool (already in the espressif/idf image) onto
# PATH — needed because Bitbucket Pipelines runs step scripts directly,
# bypassing the image's entrypoint that would normally do this.
if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
  # shellcheck disable=SC1091
  . "${IDF_PATH}/export.sh"
fi

idf.py set-target esp32c6
idf.py build

# idf.py merge-bin isn't a real subcommand on IDF v5.2.3 (it's passed
# through to CMake as an unknown target and accepts no options at all —
# confirmed via `idf.py merge-bin --help`), so call esptool's merge_bin
# directly instead. Offsets/flash args below match exactly what idf.py
# build itself prints as the "write_flash" command for this project.
python -m esptool --chip esp32c6 merge_bin \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  -o build/fleet-telematics-node-merged.bin \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/fleet_v2_master.bin

DIST="${ROOT}/dist/${FW_VERSION}"
mkdir -p "$DIST"
# The Trafyn publish step runs in a separate container and only inherits
# the dist/ artifact, not this script's shell variables — so record the
# version where it can find it without recomputing it.
echo "$FW_VERSION" > "${ROOT}/dist/VERSION"
cp build/fleet_v2_master.bin "$DIST/fleet-telematics-node-${FW_VERSION}.bin"
cp build/fleet-telematics-node-merged.bin "$DIST/fleet-telematics-node-${FW_VERSION}-merged.bin"
cp build/bootloader/bootloader.bin "$DIST/bootloader-${FW_VERSION}.bin"
cp build/partition_table/partition-table.bin "$DIST/partition-table-${FW_VERSION}.bin"
( cd "$DIST" && sha256sum ./*.bin > SHA256SUMS )

echo "$FW_VERSION"
