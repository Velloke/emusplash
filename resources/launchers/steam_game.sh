#!/usr/bin/env bash
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  steam_game.sh
#
#  Starts a Steam game and waits for it, so EmuSPLASH waits too (its hotkeys work and it comes
#  back when the game ends). "steam steam://rungameid/ID" ends at once when Steam is running.
#
#  The processes of the game are found by the Steam AppId that Steam gives them all in their
#  environment (SteamAppId=ID): the game, Proton and Wine, and launchers like Ubisoft Connect,
#  which aren't always under Steam's reaper. The game itself is the one whose program is in
#  steamapps/common (not Proton or Steam's runtime there), the reaper may end long before it.
#  When the game has ended, everything left of it is ended after 13 s (asked first, then
#  killed). Leftovers of an earlier start are ended before the start
#  ("Game already running" in Steam). Steam itself stays.
#
#  While it starts, EMUSPLASH_LAUNCH_STATUS gets a line for EmuSPLASH to show ("Steam is
#  starting in the background..."), or "GAME pid pid-in-its-flatpak" once the game runs.
#
#  Usage: steam_game.sh APPID COMMAND...   (the command of the .desktop file)
#  The steps are written to EMUSPLASH_LAUNCH_LOG if it's set.
#

APPID="$1"
shift
LOG="${EMUSPLASH_LAUNCH_LOG:-/dev/null}"
STATUS="${EMUSPLASH_LAUNCH_STATUS:-}"
NAME="${EMUSPLASH_GAME_NAME:-the game}"
CLOSE_AFTER=13
START_TIMEOUT=300

log() {
    echo "$(date '+%b %d %H:%M:%S') steam_game $APPID: $*" >> "$LOG"
}

LAST_STATUS=""
status() {
    [ -z "$STATUS" ] && return
    [ "$1" = "$LAST_STATUS" ] && return
    LAST_STATUS="$1"
    echo "$1" > "$STATUS.new" && mv -f "$STATUS.new" "$STATUS"
}

# The processes of the game: Steam's AppId in their environment.
game_pids() {
    grep -lzxE "(SteamAppId|SteamGameId|STEAM_COMPAT_APP_ID)=$APPID" /proc/[0-9]*/environ \
        2> /dev/null | cut -d/ -f3 | grep -vx -e "$$" -e "$BASHPID"
}

# The reaper of the game, empty if it isn't running.
reaper() {
    local pid cmd
    for pid in $(game_pids); do
        cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2> /dev/null)"
        case "$cmd" in
            *"SteamLaunch AppId=$APPID "* | *"SteamLaunch AppId=$APPID")
                echo "$pid"
                return
                ;;
        esac
    done
}

# The game itself among the processes: the one whose program is in the folder of the games,
# steamapps/common (S:\steamapps\common\... in Proton), but not Proton or Steam's runtime
# there. Launchers (Ubisoft Connect ...) are elsewhere. Its pid and name, or empty.
game_of() {
    local pid program
    for pid in $1; do
        program="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2> /dev/null | head -n 1)"
        case "$program" in
            *steamapps[/\\]common[/\\]Proton* | *steamapps[/\\]common[/\\]SteamLinuxRuntime* | \
                *steamapps[/\\]common[/\\]Steamworks\ Shared*) ;;
            *steamapps[/\\]common[/\\]*)
                echo "$pid $(cat "/proc/$pid/comm" 2> /dev/null)"
                return
                ;;
        esac
    done
}

# A launcher of the game among its processes, by name ("Ubisoft Connect"), or empty.
launcher() {
    local pid name
    for pid in $1; do
        name="$(cat "/proc/$pid/comm" 2> /dev/null)"
        case "$name" in
            upc.exe | Ubisoft*) echo "Ubisoft Connect"; return ;;
            EADesktop* | EALauncher* | Link2EA* | EABackground*) echo "The EA app"; return ;;
            Rockstar* | LauncherPatcher*) echo "The Rockstar Games Launcher"; return ;;
            EpicGames*) echo "The Epic Games Launcher"; return ;;
            Battle.net*) echo "Battle.net"; return ;;
            GalaxyClient* | GOG*) echo "GOG Galaxy"; return ;;
        esac
    done
}

# Ends all processes of the game, asking first, then for sure.
end_game() {
    local pids
    pids="$(game_pids)"
    [ -z "$pids" ] && return
    log "ending $(echo $pids | wc -w) processes: $*"
    kill -TERM $pids 2> /dev/null
    for _ in $(seq 1 10); do
        sleep 0.5
        [ -z "$(game_pids)" ] && return
    done
    pids="$(game_pids)"
    [ -n "$pids" ] && log "killing $(echo $pids | wc -w) processes" && kill -KILL $pids 2> /dev/null
}

# EmuSPLASH ends this script to close the game (the close game hotkey).
on_close() {
    end_game "close game"
    log "ended"
    exit 0
}
trap on_close TERM INT HUP

log "starting: $*"
if [ -n "$(game_pids)" ]; then
    status "Closing what's left of $NAME..."
    end_game "left from an earlier start"
fi

if pgrep -x steam > /dev/null || pgrep -f steamwebhelper > /dev/null; then
    status "Steam is starting $NAME..."
else
    status "Steam is starting in the background..."
fi
# Steam isn't a part of the game: its own session, no pipe to EmuSPLASH.
setsid -f "$@" > /dev/null 2>&1 < /dev/null

# Steam may start, update or sync first.
REAPER=""
for _ in $(seq 1 $((START_TIMEOUT * 2))); do
    REAPER="$(reaper)"
    [ -n "$REAPER" ] && break
    sleep 0.5
done
if [ -z "$REAPER" ]; then
    log "the game didn't start in $START_TIMEOUT s"
    exit 1
fi
log "running (reaper $REAPER)"

GAME_PID=0
GAME_NAME=""
ENDED_AT=0
STARTED_AT=$SECONDS
while true; do
    PIDS="$(game_pids)"
    [ -z "$PIDS" ] && break
    read -r pid name <<< "$(game_of "$PIDS")"
    if [ -n "$pid" ]; then
        if [ "$pid" != "$GAME_PID" ]; then
            log "the game is $name ($pid)"
            GAME_PID=$pid
            GAME_NAME=$name
        fi
        ENDED_AT=0
        # In a Flatpak (Steam's), the window of the game has the pid of the game in there.
        status "GAME $GAME_PID $(awk '/^NSpid:/ { print $NF }' "/proc/$GAME_PID/status" 2> /dev/null)"
    elif [ "$GAME_PID" != 0 ]; then
        # The game has ended, what's left are launchers.
        if [ "$ENDED_AT" = 0 ]; then
            ENDED_AT=$SECONDS
            LEFT="$(launcher "$PIDS")"
            log "$GAME_NAME has ended, closing what's left in $CLOSE_AFTER s"
            [ -z "$LEFT" ] && LEFT="what's left of $NAME"
            status "Closing $LEFT..."
        elif [ $((SECONDS - ENDED_AT)) -ge "$CLOSE_AFTER" ]; then
            end_game "left after the game"
            break
        fi
    elif [ $((SECONDS - STARTED_AT)) -ge "$START_TIMEOUT" ]; then
        # The reaper often ends early (a launcher like Ubisoft Connect takes over), so only
        # the game itself is waited for. Nothing is ended, the game may not have been found.
        log "the game didn't show up in $START_TIMEOUT s, not waiting for it"
        break
    else
        LEFT="$(launcher "$PIDS")"
        if [ -n "$LEFT" ]; then
            status "$LEFT is starting in the background..."
        else
            status "Starting $NAME..."
        fi
    fi
    sleep 1 &
    wait $!
done
log "ended"
exit 0
