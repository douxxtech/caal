#!/bin/bash

# CaaL - Container as a Login
# setup: install dependencies, build, install, and set up default bundle
# https://github.com/douxxtech/caal

set -e

# ============================================================================
# CONSTANTS
# ============================================================================

readonly RED='\033[0;31m'
readonly GRN='\033[0;32m'
readonly YEL='\033[1;33m'
readonly NC='\033[0m'

readonly DEFAULT_BUNDLE="/opt/caal/bundles/default"
readonly CONFIG_PATH="/etc/caal/caal.toml"

# ============================================================================
# UTILITY
# ============================================================================

log() {
    local level="$1"; shift
    local color=""
    case "$level" in
        INFO)  color="$GRN" ;;
        WARN)  color="$YEL" ;;
        ERROR) color="$RED" ;;
        *)     color="$NC"  ;;
    esac
    printf "[%s] ${color}%-5s${NC} %s\n" "$(date +%T)" "$level" "$*" >&2
}

require_root() {
    if [[ "$EUID" -ne 0 ]]; then
        log ERROR "Must be run as root."
        exit 1
    fi
}

# ============================================================================
# DEPENDENCIES
# ============================================================================

install_deps() {
    log INFO "Detecting package manager..."

    if command -v apt-get &>/dev/null; then
        log INFO "Debian/Ubuntu detected"
        apt-get update -qq
        apt-get install -y gcc make crun skopeo umoci

    elif command -v dnf &>/dev/null; then
        log INFO "Fedora/RHEL detected"
        dnf install -y gcc make crun skopeo umoci

    elif command -v yum &>/dev/null; then
        log INFO "CentOS/older RHEL detected"
        yum install -y gcc make crun skopeo umoci

    elif command -v pacman &>/dev/null; then
        log INFO "Arch Linux detected"
        pacman -Sy --noconfirm gcc make crun skopeo umoci

    elif command -v zypper &>/dev/null; then
        log INFO "openSUSE detected"
        zypper install -y gcc make crun skopeo umoci

    else
        log ERROR "Could not detect a supported package manager."
        log ERROR "Please manually install: gcc, make, crun, skopeo, umoci"
        exit 1
    fi
}

# ============================================================================
# BUILD & INSTALL
# ============================================================================

build_and_install() {
    log INFO "Building caalsh..."
    make clean
    make

    log INFO "Installing (make install)..."
    make install

    # Register caalsh as a valid shell
    if ! grep -qxF "/usr/local/bin/caalsh" /etc/shells; then
        log INFO "Adding caalsh to /etc/shells"
        echo "/usr/local/bin/caalsh" >> /etc/shells
    else
        log WARN "caalsh already in /etc/shells, skipping"
    fi
}

# ============================================================================
# DEFAULT BUNDLE
# ============================================================================

create_default_bundle() {
    log INFO "Pulling busybox image via skopeo..."

    local oci_tmp="/tmp/caal-busybox-oci"
    rm -rf "$oci_tmp"

    skopeo copy docker://busybox:latest "oci:$oci_tmp:latest"

    log INFO "Unpacking bundle to $DEFAULT_BUNDLE via umoci..."
    mkdir -p "$(dirname "$DEFAULT_BUNDLE")"
    umoci unpack --image "$oci_tmp:latest" "$DEFAULT_BUNDLE"

    rm -rf "$oci_tmp"

    # Base caal.toml with a header (no user entries yet)
    if [[ ! -f "$CONFIG_PATH" ]]; then
        log INFO "Creating base config at $CONFIG_PATH"
        mkdir -p "$(dirname "$CONFIG_PATH")"
        cat > "$CONFIG_PATH" <<EOF
# CaaL configuration
# Each section maps a unix user to an OCI bundle.
# Format:
# [<user>]
# bundle = /full/path/to/bundle
# timeout = timeout_seconds # 0 = no timeout
# enabled = true|false

EOF
    else
        log WARN "$CONFIG_PATH already exists, skipping"
    fi

    log INFO "Default bundle ready at $DEFAULT_BUNDLE"
}

# ============================================================================
# MAIN
# ============================================================================

main() {
    require_root

    log INFO "> CaaL setup <"
    echo ""

    install_deps
    echo ""

    build_and_install
    echo ""

    create_default_bundle
    echo ""

    log INFO "> Setup complete <"
    log INFO "Use 'newcaal <username>' to create your first CaaL user."
}

main "$@"