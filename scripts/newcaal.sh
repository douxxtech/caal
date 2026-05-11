#!/bin/bash

# CaaL - Container as a Login
# newcaal: create a new CaaL user
# Usage: newcaal <username> [-y]
# https://github.com/douxxtech/caal

set -e

# ============================================================================
# CONSTANTS
# ============================================================================

readonly RED='\033[0;31m'
readonly GRN='\033[0;32m'
readonly YEL='\033[1;33m'
readonly NC='\033[0m'

readonly CONFIG_PATH="/etc/caal/caal.toml"
readonly CAALSH_PATH="/usr/local/bin/caalsh"

# Defaults (used with -y)
readonly DEFAULT_BUNDLE="/opt/caal/bundles/default"
readonly DEFAULT_TIMEOUT=0
readonly DEFAULT_ENABLED="true"

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

ask() {
    # Prompt with an optional default shown in brackets.
    # Usage: ask "Prompt text" [default]
    # Returns answer in $REPLY, or $default if user hits enter with no input.
    local prompt="$1"
    local default="$2"

    if [[ -n "$default" ]]; then
        printf "${GRN}[INPUT]${NC} %s [%s]: " "$prompt" "$default" >&2
    else
        printf "${GRN}[INPUT]${NC} %s: " "$prompt" >&2
    fi

    read -r REPLY
    [[ -z "$REPLY" && -n "$default" ]] && REPLY="$default" || true
}

# ============================================================================
# INTERACTIVE MENU
# Credit: https://unix.stackexchange.com/questions/146570/arrow-key-enter-menu
# ============================================================================

select_option() {
    set +e

    if [[ -e /dev/tty ]]; then
        exec < /dev/tty > /dev/tty
    elif [[ ! -t 0 ]] || [[ ! -t 1 ]]; then
        _fallback_menu "$@"
        return $?
    fi

    _arrow_key_menu "$@"
    local result=$?
    return $result
}

_fallback_menu() {
    local idx=1
    for opt; do
        echo "  $idx) $opt" >&2
        ((idx++))
    done

    while true; do
        printf "Enter number (1-$#): " >&2
        read selection
        if [[ "$selection" =~ ^[0-9]+$ ]] && [ "$selection" -ge 1 ] && [ "$selection" -le $# ]; then
            return $((selection - 1))
        fi
        echo "Invalid selection." >&2
    done
}

_arrow_key_menu() {
    local ESC=$(printf "\033")
    local SEL_COLOR="${ESC}[94m"

    cursor_blink_on()  { printf "${ESC}[?25h"; }
    cursor_blink_off() { printf "${ESC}[?25l"; }
    cursor_to()        { printf "${ESC}[$1;${2:-1}H"; }
    print_option()     { printf "  $1${NC}"; }
    print_selected()   { printf "${SEL_COLOR}> $1${NC}"; }
    get_cursor_row()   { IFS=';' read -sdR -p $'\E[6n' ROW COL; echo ${ROW#*[}; }
    key_input() {
        read -s -n3 key 2>/dev/null >&2
        if [[ $key = "${ESC}[A" ]]; then echo up; fi
        if [[ $key = "${ESC}[B" ]]; then echo down; fi
        if [[ $key = "" ]];         then echo enter; fi
    }

    for opt; do printf "\n"; done

    local lastrow=$(get_cursor_row)
    local startrow=$(( lastrow - $# ))

    trap "cursor_blink_on; stty echo; printf '\n'; exit" 2
    cursor_blink_off

    local selected=0
    while true; do
        local idx=0
        for opt; do
            cursor_to $(( startrow + idx ))
            if [ $idx -eq $selected ]; then print_selected "$opt"; else print_option "$opt"; fi
            ((idx++))
        done

        case $(key_input) in
            enter) break ;;
            up)    (( selected-- )); [ $selected -lt 0 ] && selected=$(( $# - 1 )) ;;
            down)  (( selected++ )); [ $selected -ge $# ] && selected=0 ;;
        esac
    done

    cursor_to $lastrow
    printf "\n"
    cursor_blink_on
    return $selected
}

# ============================================================================
# ACTIONS
# ============================================================================

create_user() {
    local username="$1"

    if id "$username" &>/dev/null; then
        log WARN "User '$username' already exists, skipping creation"
    else
        log INFO "Creating system user: $username"
        useradd \
            --home-dir /tmp \
            --shell "$CAALSH_PATH" \
            "$username"

        log INFO "Set a password for '$username':"
        passwd "$username"
    fi
}

write_toml() {
    local username="$1"
    local bundle="$2"
    local timeout="$3"
    local enabled="$4"

    log INFO "Writing config entry to $CONFIG_PATH"
    mkdir -p "$(dirname "$CONFIG_PATH")"

    # Create file with header if it doesn't exist yet
    if [[ ! -f "$CONFIG_PATH" ]]; then
        cat > "$CONFIG_PATH" <<EOF
# CaaL configuration
# Each section maps a unix user to an OCI bundle.

EOF
    fi

    # Remove existing block for this user if present
    if grep -q "^\[$username\]" "$CONFIG_PATH" 2>/dev/null; then
        log WARN "Existing config for '$username' found, replacing it"
        sed -i "/^\[$username\]/,/^\[/{//!d}; /^\[$username\]/d" "$CONFIG_PATH"
    fi

    cat >> "$CONFIG_PATH" <<EOF

[$username]
bundle  = "$bundle"
enabled = $enabled
timeout = $timeout
EOF
}

write_sshd() {
    local username="$1"
    local conf="/etc/ssh/sshd_config.d/caal-${username}.conf"

    log INFO "Writing sshd config to $conf"
    mkdir -p /etc/ssh/sshd_config.d
    cat > "$conf" <<EOF
# Generated by newcaal for user: $username
Match User $username
    AllowAgentForwarding no
    AllowTcpForwarding no
    X11Forwarding no
    Banner none
    ForceCommand $CAALSH_PATH
EOF

    local main_cfg="/etc/ssh/sshd_config"
    if ! grep -qE '^\s*Include\s+/etc/ssh/sshd_config\.d/\*\.conf' "$main_cfg" 2>/dev/null; then
            log WARN "Main sshd config does not include sshd_config.d, adding it..."
            sed -i '1s|^|Include /etc/ssh/sshd_config.d/*.conf\n|' "$main_cfg"
            log INFO "Added Include directive to $main_cfg"
        fi

    log INFO "Reloading sshd..."
    systemctl reload sshd
}

# ============================================================================
# MAIN
# ============================================================================

main() {
    # Parse args
    local username=""
    local yes_mode=false

    for arg in "$@"; do
        case "$arg" in
            -y|--yes) yes_mode=true ;;
            -*)
                log ERROR "Unknown option: $arg"
                echo "Usage: newcaal <username> [-y]" >&2
                exit 1
                ;;
            *)
                if [[ -z "$username" ]]; then
                    username="$arg"
                else
                    log ERROR "Unexpected argument: $arg"
                    echo "Usage: newcaal <username> [-y]" >&2
                    exit 1
                fi
                ;;
        esac
    done

    if [[ -z "$username" ]]; then
        log ERROR "No username provided."
        echo "Usage: newcaal <username> [-y]" >&2
        exit 1
    fi

    if [[ "$EUID" -ne 0 ]]; then
        log ERROR "Must be run as root."
        exit 1
    fi

    # Gather config — skip prompts in -y mode
    local bundle timeout enabled

    if [[ "$yes_mode" == true ]]; then
        bundle="$DEFAULT_BUNDLE"
        timeout="$DEFAULT_TIMEOUT"
        enabled="$DEFAULT_ENABLED"
        log INFO "Using defaults for '$username' (-y)"
    else
        ask "OCI bundle path" "$DEFAULT_BUNDLE"
        bundle="$REPLY"
        if [[ "$bundle" != /* ]]; then
            log ERROR "Bundle path must be absolute."
            exit 1
        fi

        ask "Session timeout in seconds (0 = none)" "$DEFAULT_TIMEOUT"
        timeout="$REPLY"
        if ! [[ "$timeout" =~ ^[0-9]+$ ]]; then
            log ERROR "Timeout must be a non-negative integer."
            exit 1
        fi

        log INFO "Enable user immediately?"
        enabled="true"
        select_option "Yes" "No"
        [[ $? -eq 1 ]] && enabled="false"
        set -e
    fi

    # Summary
    echo ""
    log INFO "Creating user '$username' with:"
    log INFO "  Bundle  : $bundle"
    log INFO "  Timeout : $timeout"
    log INFO "  Enabled : $enabled"
    echo ""

    create_user  "$username"
    write_toml   "$username" "$bundle" "$timeout" "$enabled"
    write_sshd   "$username"

    log INFO "Done. '$username' is ready."
}

main "$@"
