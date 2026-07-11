#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-run}"
APP_NAME="SonyHeadTracker"
BUNDLE_ID="io.github.NicholasSlattery.SonyHeadTracker"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DERIVED_DATA="$ROOT_DIR/build/DerivedData"
PROJECT="$ROOT_DIR/macos/SonyHeadTracker.xcodeproj"
APP_BUNDLE="$DERIVED_DATA/Build/Products/Debug/$APP_NAME.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
DEBUG_ENTITLEMENTS="$ROOT_DIR/macos/Debug.entitlements"
export DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"

stable_sign_if_available() {
  if [[ "${SHT_SKIP_STABLE_SIGNING:-0}" == "1" ]]; then
    return
  fi

  local identity="${SHT_CODE_SIGN_IDENTITY:-}"
  if [[ -z "$identity" ]]; then
    local identities=()
    while IFS= read -r hash; do
      [[ -n "$hash" ]] && identities+=("$hash")
    done < <(/usr/bin/security find-identity -p codesigning -v 2>/dev/null \
      | /usr/bin/sed -nE 's/^[[:space:]]*[0-9]+\) ([0-9A-F]+) "Apple Development:.*$/\1/p')

    if [[ ${#identities[@]} -eq 1 ]]; then
      identity="${identities[0]}"
    elif [[ ${#identities[@]} -gt 1 ]]; then
      echo "Multiple Apple Development identities found; set SHT_CODE_SIGN_IDENTITY to select one." >&2
    fi
  fi

  if [[ -n "$identity" ]]; then
    /usr/bin/codesign --force --deep --entitlements "$DEBUG_ENTITLEMENTS" \
      --sign "$identity" --timestamp=none "$APP_BUNDLE"
    echo "Signed with a stable development identity so Input Monitoring survives rebuilds."
  else
    echo "No unique Apple Development identity found; using Xcode's ad-hoc signature." >&2
    echo "Input Monitoring may need to be granted again after rebuilding." >&2
  fi
}

pkill -x "$APP_NAME" >/dev/null 2>&1 || true

/usr/bin/xcodebuild \
  -project "$PROJECT" \
  -scheme "$APP_NAME" \
  -configuration Debug \
  -derivedDataPath "$DERIVED_DATA" \
  build

stable_sign_if_available

open_app() {
  /usr/bin/open -n "$APP_BUNDLE"
}

case "$MODE" in
  run)
    open_app
    ;;
  --debug|debug)
    exec lldb -- "$APP_BINARY"
    ;;
  --logs|logs)
    open_app
    exec /usr/bin/log stream --info --style compact --predicate "process == \"$APP_NAME\""
    ;;
  --telemetry|telemetry)
    open_app
    exec /usr/bin/log stream --info --style compact --predicate "subsystem == \"$BUNDLE_ID\""
    ;;
  --verify|verify)
    open_app
    sleep 1
    pgrep -x "$APP_NAME" >/dev/null
    ;;
  *)
    echo "usage: $0 [run|--debug|--logs|--telemetry|--verify]" >&2
    exit 2
    ;;
esac
