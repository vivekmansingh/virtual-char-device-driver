#!/usr/bin/env bash
# mydev.sh - Load / unload the mydev kernel module and manage permissions.
# ========================================================================
#
# Usage:
#   sudo ./mydev.sh load     # install udev rule, insert module, check /dev/mydev
#   sudo ./mydev.sh unload   # remove module and udev rule
#   sudo ./mydev.sh reload   # unload (if loaded) then load
#        ./mydev.sh status   # show module, device node and recent kernel logs
#
# WHY A UDEV RULE?
#   When the module calls device_create(), the kernel announces the new
#   device and the "udev" service creates /dev/mydev.  By default udev makes
#   it owned by root with mode 0600, so a normal user's ./app would get
#   "Permission denied".  We install a small rule file telling udev to use
#   mode 0666 (read+write for everyone) for this one device.
#   (0666 is fine for a learning project; a real driver would usually give
#    access to a specific group instead.)

# Bash "strict mode":
#   -e  exit immediately if a command fails
#   -u  treat use of an unset variable as an error
#   -o pipefail  a pipeline fails if ANY command in it fails
set -euo pipefail

# ---- Configuration -------------------------------------------------------
MODULE_NAME="mydev"
# Directory this script lives in (so it works from any current directory).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_FILE="${SCRIPT_DIR}/${MODULE_NAME}.ko"
DEVICE_NODE="/dev/${MODULE_NAME}"
UDEV_RULE_FILE="/etc/udev/rules.d/99-${MODULE_NAME}.rules"
# The rule: for the kernel device named "mydev", set permissions to 0666.
UDEV_RULE='KERNEL=="mydev", MODE="0666"'

# ---- Helper functions ----------------------------------------------------

# Print a message prefixed with the script name.
info()  { echo "[mydev.sh] $*"; }
# Print an error to stderr and exit with failure.
error() { echo "[mydev.sh] ERROR: $*" >&2; exit 1; }

# Loading modules needs root.  If we are not root, re-run ourselves via sudo.
# $EUID is the effective user id; root is 0.  "$@" passes the same arguments.
require_root() {
    if [[ ${EUID} -ne 0 ]]; then
        info "Root privileges required - re-running with sudo..."
        exec sudo "$0" "$@"
    fi
}

# A loaded module appears as a directory under /sys/module/.
is_loaded() {
    [[ -d "/sys/module/${MODULE_NAME}" ]]
}

# ---- Commands ------------------------------------------------------------

do_load() {
    if is_loaded; then
        info "Module '${MODULE_NAME}' is already loaded."
        return 0
    fi

    # The module must have been built first.
    [[ -f "${MODULE_FILE}" ]] || error "${MODULE_FILE} not found. Run 'make' first."

    # 1) Install the udev permission rule and tell udev to re-read its rules.
    info "Installing udev rule -> ${UDEV_RULE_FILE}"
    printf '%s\n' "${UDEV_RULE}" > "${UDEV_RULE_FILE}"
    udevadm control --reload-rules

    # 2) Insert the module into the running kernel (runs mydev_init()).
    info "Inserting ${MODULE_FILE}"
    insmod "${MODULE_FILE}"

    # 3) Wait for udev to finish processing the new device event.
    udevadm settle || true

    # Give udev up to ~5 seconds to create the device file.
    for _ in $(seq 1 50); do
        [[ -e "${DEVICE_NODE}" ]] && break
        sleep 0.1
    done
    [[ -e "${DEVICE_NODE}" ]] || error "${DEVICE_NODE} was not created (check 'sudo dmesg')."

    # 4) Safety net: if udev did not apply the rule for some reason,
    #    set the permissions ourselves.  stat -c '%a' prints the mode, e.g. 666.
    if [[ "$(stat -c '%a' "${DEVICE_NODE}")" != "666" ]]; then
        info "udev rule not applied yet - setting permissions manually."
        chmod 0666 "${DEVICE_NODE}"
    fi

    info "Loaded. Device node:"
    ls -l "${DEVICE_NODE}"
    info "Latest kernel messages:"
    dmesg | grep "mydev" | tail -n 3 || true
}

do_unload() {
    if is_loaded; then
        # Remove the module (runs mydev_exit(), which deletes /dev/mydev).
        # This fails with "Module is in use" if a program still has it open.
        info "Removing module '${MODULE_NAME}'"
        rmmod "${MODULE_NAME}"
    else
        info "Module '${MODULE_NAME}' is not loaded."
    fi

    # Remove our udev rule so the system is left exactly as we found it.
    if [[ -f "${UDEV_RULE_FILE}" ]]; then
        info "Removing udev rule ${UDEV_RULE_FILE}"
        rm -f "${UDEV_RULE_FILE}"
        udevadm control --reload-rules
    fi

    info "Unloaded."
    dmesg | grep "mydev" | tail -n 2 || true
}

do_status() {
    if is_loaded; then
        info "Module is LOADED:"
        lsmod | grep "^${MODULE_NAME} " || true
    else
        info "Module is NOT loaded."
    fi

    if [[ -e "${DEVICE_NODE}" ]]; then
        info "Device node:"
        ls -l "${DEVICE_NODE}"
    else
        info "${DEVICE_NODE} does not exist."
    fi

    # The major number assigned to us is listed in /proc/devices.
    grep " ${MODULE_NAME}\$" /proc/devices || true

    info "Recent kernel log lines (may need sudo):"
    dmesg 2>/dev/null | grep "mydev" | tail -n 10 || true
}

usage() {
    echo "Usage: $0 {load|unload|reload|status}"
    exit 1
}

# ---- Main: dispatch on the first argument --------------------------------
# "${1:-}" means "first argument, or empty string if none" (safe with set -u).
case "${1:-}" in
    load)   require_root "$@"; do_load ;;
    unload) require_root "$@"; do_unload ;;
    reload) require_root "$@"; do_unload; do_load ;;
    status) do_status ;;
    *)      usage ;;
esac
