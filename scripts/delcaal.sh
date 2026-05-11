#!/bin/bash

# CaaL - Container as a Login
# delcaal: delete a CaaL user
# Usage: delcaal <username> [-y]
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

remove_user() {
    local username="$1"

    if ! id "$username" &>/dev/null; then
        log WARN "User '$username' does not exist, skipping"
    else
        log INFO "Deleting system user: $username"
        userdel "$username"
    fi
}

remove_toml() {
    local username="$1"

    if [[ ! -f "$CONFIG_PATH" ]]; then
        log WARN "Config file not found, skipping TOML cleanup"
        return
    fi

    if ! grep -q "^\[$username\]" "$CONFIG_PATH" 2>/dev/null; then
        log WARN "No config entry found for '$username', skipping"
        return
    fi

    log INFO "Removing config entry for '$username' from $CONFIG_PATH"
    sed -i "/^\[$username\]/,/^\[/{//!d}; /^\[$username\]/d" "$CONFIG_PATH"
}

remove_sshd() {
    local username="$1"
    local conf="/etc/ssh/sshd_config.d/caal-${username}.conf"

    if [[ ! -f "$conf" ]]; then
        log WARN "No sshd config found for '$username', skipping"
        return
    fi

    log INFO "Removing sshd config: $conf"
    rm -f "$conf"

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
                echo "Usage: delcaal <username> [-y]" >&2
                exit 1
                ;;
            *)
                if [[ -z "$username" ]]; then
                    username="$arg"
                else
                    log ERROR "Unexpected argument: $arg"
                    echo "Usage: delcaal <username> [-y]" >&2
                    exit 1
                fi
                ;;
        esac
    done

    if [[ -z "$username" ]]; then
        log ERROR "No username provided."
        echo "Usage: delcaal <username> [-y]" >&2
        exit 1
    fi

    if [[ "$EUID" -ne 0 ]]; then
        log ERROR "Must be run as root."
        exit 1
    fi

    # Confirm (skip in -y mode))
    if [[ "$yes_mode" == false ]]; then
        echo ""
        log WARN "This will permanently delete user '$username' and all associated CaaL config."
        log INFO "Are you sure?"
        select_option "Yes" "No"
        [[ $? -eq 1 ]] && { log INFO "Aborted."; exit 0; }
        set -e
    fi

    echo ""
    log INFO "Deleting user '$username'..."
    echo ""

    remove_user  "$username"
    remove_toml  "$username"
    remove_sshd  "$username"

    log INFO "Done. '$username' has been removed."
}

main "$@"