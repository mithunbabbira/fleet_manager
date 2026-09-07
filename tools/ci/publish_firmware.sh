#!/usr/bin/env bash
# Register the just-built firmware with Trafyn's publish-device-firmware
# workflow, so the device's own OTA check (components/ota/ota_cloud.c,
# which POSTs get-latest-device-firmware with manufacturer/deviceType) can
# find it.
#
# manufacturer/deviceType below MUST match components/ota/Kconfig's
# OTA_CLOUD_MANUFACTURER / OTA_CLOUD_DEVICE_TYPE defaults exactly
# ("Espressif Systems" / "fleet monitor") — Trafyn matches on these, so a
# mismatch means devices never see this build as available.
#
# Requires (set as secured Bitbucket Deployment variables, per environment):
#   TRAFYN_AUTH_USER, TRAFYN_AUTH_PASS   - HTTP Basic Auth for the publish API
#   TRAFYN_SYSTEM_USER_ID                - x-nc-system-user-id header value
#
# Expects dist/VERSION and dist/<version>/fleet-telematics-node-<version>.bin
# to already exist (written by build_firmware.sh in an earlier step).
#
# Usage: publish_firmware.sh <environment>   (environment: dev | staging | prod)
set -euo pipefail

ENVIRONMENT="${1:?usage: publish_firmware.sh <environment>}"
: "${TRAFYN_AUTH_USER:?TRAFYN_AUTH_USER not set}"
: "${TRAFYN_AUTH_PASS:?TRAFYN_AUTH_PASS not set}"
: "${TRAFYN_SYSTEM_USER_ID:?TRAFYN_SYSTEM_USER_ID not set}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

FW_VERSION="$(tr -d '[:space:]' < dist/VERSION)"
BIN_PATH="dist/${FW_VERSION}/fleet-telematics-node-${FW_VERSION}.bin"
BIN_NAME="$(basename "$BIN_PATH")"
if [[ ! -f "$BIN_PATH" ]]; then
  echo "missing $BIN_PATH — did the build step run first?" >&2
  exit 1
fi

SHA256="$(sha256sum "$BIN_PATH" | awk '{print $1}')"
SIZE="$(wc -c < "$BIN_PATH" | tr -d '[:space:]')"

REQUEST_JSON="$(mktemp)"
RESPONSE_BODY="$(mktemp)"
trap 'rm -f "$REQUEST_JSON" "$RESPONSE_BODY"' EXIT

cat > "$REQUEST_JSON" <<JSON
{
  "workflowName": "publish-device-firmware",
  "dataMap": {
    "input": {
      "environment": "${ENVIRONMENT}",
      "manufacturer": "Espressif Systems",
      "deviceType": "fleet monitor",
      "latestVersion": "${FW_VERSION}",
      "fileName": "${BIN_NAME}",
      "sha256": "${SHA256}"
    }
  },
  "fileMetaData": [{"filename": "${BIN_NAME}", "size": ${SIZE}}]
}
JSON

echo "Publishing ${BIN_NAME} (${FW_VERSION}, sha256=${SHA256}, ${SIZE} bytes) to Trafyn as environment=${ENVIRONMENT}"

HTTP_STATUS="$(curl -sS -o "$RESPONSE_BODY" -w '%{http_code}' --location \
  'https://api.trafyn.info/workflow-engine/1/1/v1/execution/request/multipart?refreshCache=true' \
  -u "${TRAFYN_AUTH_USER}:${TRAFYN_AUTH_PASS}" \
  --header "x-nc-system-user-id: ${TRAFYN_SYSTEM_USER_ID}" \
  --form "request=<${REQUEST_JSON};type=application/json" \
  --form "files=@${BIN_PATH};filename=${BIN_NAME}")"

cat "$RESPONSE_BODY"
echo

if [[ "$HTTP_STATUS" -lt 200 || "$HTTP_STATUS" -ge 300 ]]; then
  echo "Trafyn publish failed (HTTP ${HTTP_STATUS})" >&2
  exit 1
fi
