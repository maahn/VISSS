#!/bin/bash
# Builds the visss-data-acquisition-EVT client and its plugins (motion_detect, record), then
# installs the plugins where eCaptureProServer loads them from and restarts that service so it
# picks them up.
#
# Usage:
#   ./build_and_install.sh              # build all, install plugins, restart the server
#   ./build_and_install.sh --no-install # build all, skip the sudo install/restart steps
#   ./build_and_install.sh --clean      # wipe existing build/ dirs first (implies a full rebuild)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${SCRIPT_DIR}/visss-data-acquisition-EVT"
# plugin directory name == its CMake project name == its lib<name>.so filename, for all of these.
PLUGIN_NAMES=(motion_detect record)
PLUGINS_INSTALL_DIR="/opt/EVT/eCapturePro/eSdkPro/plugins"
SERVICE_NAME="evt_eCaptureProServer.service"

: "${EMERGENT_DIR:=/opt/EVT}"
: "${ECAPTURE_PRO_DIR:=/opt/EVT/eCapturePro}"
export EMERGENT_DIR ECAPTURE_PRO_DIR

DO_INSTALL=1
DO_CLEAN=0
for arg in "$@"; do
    case "${arg}" in
        --no-install) DO_INSTALL=0 ;;
        --clean) DO_CLEAN=1 ;;
        *)
            echo "Unknown argument: ${arg}" >&2
            echo "Usage: $0 [--no-install] [--clean]" >&2
            exit 1
            ;;
    esac
done

build_project() {
    local dir="$1"
    local name="$2"

    echo "==> Building ${name} (${dir})"
    if [[ "${DO_CLEAN}" == "1" ]]; then
        rm -rf "${dir}/build"
    fi
    cmake -S "${dir}" -B "${dir}/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${dir}/build"
}

for plugin_name in "${PLUGIN_NAMES[@]}"; do
    build_project "${SCRIPT_DIR}/${plugin_name}" "${plugin_name} plugin"
done
build_project "${APP_DIR}" "visss-data-acquisition-EVT client"

if [[ "${DO_INSTALL}" == "0" ]]; then
    echo "==> Skipping install (--no-install passed)"
    exit 0
fi

for plugin_name in "${PLUGIN_NAMES[@]}"; do
    PLUGIN_SO="${SCRIPT_DIR}/${plugin_name}/build/lib${plugin_name}.so"
    if [[ ! -f "${PLUGIN_SO}" ]]; then
        echo "Built plugin not found at ${PLUGIN_SO}" >&2
        exit 1
    fi

    echo "==> Installing ${PLUGIN_SO} to ${PLUGINS_INSTALL_DIR}/ (requires sudo)"
    cp "${PLUGIN_SO}" "${PLUGINS_INSTALL_DIR}/"
done

echo "==> Restarting ${SERVICE_NAME} (requires sudo)"
sudo systemctl restart "${SERVICE_NAME}"

echo "==> Done."
