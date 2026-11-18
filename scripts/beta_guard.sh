#!/bin/sh
# beta_guard.sh - keep a privately built FreeToon beta in place.
#
# An official FreeToon install (the update button, the nightly auto-update or
# a manual install.sh) replaces /mnt/data/toonui and /mnt/data/quby_bridge.
# This guard keeps pinned copies of the beta binaries in its own folder and
# puts them back when they get replaced, logs it and leaves a notice that the
# restored toonui shows in the inbox.
#
# It lives outside the official file set (/mnt/data/beta-pin) and runs from
# its own inittab row (btpn), which install.sh does not know and leaves alone.
#
#   beta_guard.sh setup    pin the binaries now in /mnt/data, install the
#                          script + inittab row and switch the guard on
#   beta_guard.sh off      stop restoring (deliberately back to official)
#   beta_guard.sh on       restore again
#   beta_guard.sh pin      pin what is in /mnt/data now (a new beta you
#                          installed by hand)
#   beta_guard.sh status   show state, versions and the last log lines
#   beta_guard.sh remove   switch off and drop the inittab row
#   beta_guard.sh check    one pass (what the watcher does every 20 s)
#   beta_guard.sh watch    the loop the inittab row runs
#
# A toonui carrying the own-build label (BETA_MARK, "-beta.lourens.") that is
# not the pinned one is a new beta, installed on purpose through the update
# channel "Eigen bouw": it is adopted (pinned) instead of rolled back.

PIN=/mnt/data/beta-pin
DATA=/mnt/data
LOG=$PIN/guard.log
ROW_ID=btpn
ROW="btpn:345:respawn:$PIN/beta_guard.sh watch"
BETA_MARK="${BETA_MARK:--beta.lourens.}"
FILES="toonui quby_bridge"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"
    # keep the log small (flash): last 300 lines once it passes 64 KB
    if [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 65536 ]; then
        tail -n 300 "$LOG" > "$LOG.tmp" && mv -f "$LOG.tmp" "$LOG"
    fi
}

md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# Version label baked into a toonui, e.g. 1.0.12-beta.lourens.1 ("" if none).
label_of() {
    grep -ao "[0-9][0-9.]*$BETA_MARK[0-9]*" "$1" 2>/dev/null | head -n 1
}

# An installer is busy: its download folder exists (removed when it exits)
# and is younger than 15 min (a killed installer can leave one behind).
installer_busy() {
    [ -n "$(find "$DATA" -maxdepth 1 -name '.freetoon_dl.*' -mmin -15 2>/dev/null)" ] && return 0
    # procps ps (Toon 2) needs -ef to show arguments; BusyBox ps shows them as is
    { ps -ef 2>/dev/null || ps 2>/dev/null; } | grep -v grep \
        | grep -q -e 'install\.sh' -e 'sh -s -- --update' -e 'ot_mode_switch' && return 0
    return 1
}

pin_files() {
    mkdir -p "$PIN"
    for f in $FILES; do
        [ -f "$DATA/$f" ] || continue
        cp "$DATA/$f" "$PIN/$f.tmp" && chmod 755 "$PIN/$f.tmp" && mv -f "$PIN/$f.tmp" "$PIN/$f"
        md5 "$PIN/$f" > "$PIN/$f.md5"
    done
    rm -f "$PIN/.pending"
    sync
}

notice() {
    echo "$*" > "$PIN/notice.tmp" && mv -f "$PIN/notice.tmp" "$PIN/notice"
}

# Restart quby_bridge on the restored binary. SIGTERM only: the bridge drops
# its bind-mount on exit, init respawns it from the qbri row, and the new one
# clears any stale mount, kicks happ_thermstat and verifies it landed on the
# PTY. No umount or happ_thermstat kill from here: init respawns the bridge
# within a second, and an outside umount then removes the NEW bridge's mount
# (happ_thermstat ends up on the kernel UART next to the bridge).
restart_bridge() {
    grep -q '^qbri:' /etc/inittab 2>/dev/null || return 0
    old=$(pidof quby_bridge)
    [ -n "$old" ] || return 0          # not running: init starts the new one
    kill $old 2>/dev/null
    i=0; alive=1
    while [ $i -lt 10 ]; do
        alive=0
        for p in $old; do kill -0 "$p" 2>/dev/null && alive=1; done
        [ $alive = 0 ] && break
        sleep 1; i=$((i + 1))
    done
    [ $alive = 1 ] && kill -9 $old 2>/dev/null   # new bridge drops the stale mount
    return 0
}

restore() {   # $1 = file name
    f=$1
    [ -f "$PIN/$f" ] || { log "no pinned $f, skipped"; return 1; }
    cp "$PIN/$f" "$DATA/$f.betapin" && chmod 755 "$DATA/$f.betapin" \
        && sync && mv -f "$DATA/$f.betapin" "$DATA/$f" && sync || {
        rm -f "$DATA/$f.betapin"; log "restoring $f FAILED"; return 1; }
    return 0
}

check() {
    [ -f "$PIN/ENABLED" ] || return 0
    if installer_busy; then
        [ -f "$PIN/.busy" ] || { : > "$PIN/.busy"; log "installer running, waiting"; }
        return 0
    fi
    rm -f "$PIN/.busy"

    changed=""
    sig=""
    for f in $FILES; do
        # A file that is gone was removed on purpose (uninstall): leave it.
        [ -f "$DATA/$f" ] && [ -f "$PIN/$f.md5" ] || continue
        cur=$(md5 "$DATA/$f")
        if [ "$cur" != "$(cat "$PIN/$f.md5")" ]; then
            changed="$changed $f"
            sig="$sig$f=$cur "
        fi
    done
    [ -n "$changed" ] || { rm -f "$PIN/.pending"; return 0; }

    # Act only when the same change is seen on two passes in a row, so a
    # file that is still being written is never copied over halfway.
    if [ "$(cat "$PIN/.pending" 2>/dev/null)" != "$sig" ]; then
        echo "$sig" > "$PIN/.pending"
        return 0
    fi
    rm -f "$PIN/.pending"

    newlabel=$(label_of "$DATA/toonui")
    pinlabel=$(label_of "$PIN/toonui")
    case " $changed " in *" toonui "*)
        if [ -n "$newlabel" ]; then
            log "new own beta $newlabel replaces $pinlabel: adopted (pinned)"
            pin_files
            notice "Nieuwe eigen beta $newlabel vastgezet."
            return 0
        fi ;;
    esac

    log "official install detected (changed:$changed), restoring beta $pinlabel"
    for f in $changed; do
        restore "$f" || continue
        log "restored $f ($(md5 "$DATA/$f"))"
        case $f in
            toonui)      killall -9 toonui 2>/dev/null || true ;;   # init respawns it
            quby_bridge) restart_bridge ;;
        esac
    done
    what=$(echo $changed | sed 's/ / en /')
    notice "Eigen beta $pinlabel teruggezet: een officiele update had $what vervangen. Bewust naar officieel: kies bij Update het kanaal Stabiel of Beta."
}

case "${1:-status}" in
    watch)
        mkdir -p "$PIN"
        log "watcher started (enabled=$([ -f "$PIN/ENABLED" ] && echo yes || echo no))"
        sleep 10
        while :; do check; sleep 20; done
        ;;
    check)
        check
        ;;
    setup)
        mkdir -p "$PIN"
        [ "$0" -ef "$PIN/beta_guard.sh" ] || { cp "$0" "$PIN/beta_guard.sh.tmp" \
            && chmod 755 "$PIN/beta_guard.sh.tmp" && mv -f "$PIN/beta_guard.sh.tmp" "$PIN/beta_guard.sh"; }
        pin_files
        echo "on (setup $(date '+%Y-%m-%d'))" > "$PIN/ENABLED"
        if [ -x "$DATA/inittab_edit.sh" ]; then
            "$DATA/inittab_edit.sh" upsert "$ROW_ID" "$ROW" || { echo "inittab upsert failed"; exit 1; }
        else
            echo "no $DATA/inittab_edit.sh; add this row by hand: $ROW"; exit 1
        fi
        log "setup: pinned $(label_of "$PIN/toonui") and quby_bridge $(cat "$PIN/quby_bridge.md5" 2>/dev/null)"
        echo "beta guard on; pinned toonui $(label_of "$PIN/toonui")"
        ;;
    pin)
        pin_files
        log "pinned by hand: toonui $(label_of "$PIN/toonui"), quby_bridge $(cat "$PIN/quby_bridge.md5" 2>/dev/null)"
        echo "pinned"
        ;;
    on)
        echo "on ($(date '+%Y-%m-%d %H:%M'))" > "$PIN/ENABLED"; sync
        log "switched on"; echo "beta guard on"
        ;;
    off)
        rm -f "$PIN/ENABLED" "$PIN/.pending"; sync
        log "switched off: official updates are no longer rolled back"; echo "beta guard off"
        ;;
    remove)
        rm -f "$PIN/ENABLED"; sync
        [ -x "$DATA/inittab_edit.sh" ] && "$DATA/inittab_edit.sh" drop "$ROW_ID"
        log "removed inittab row"; echo "beta guard removed (pinned files kept in $PIN)"
        ;;
    status)
        echo "guard:   $([ -f "$PIN/ENABLED" ] && echo ON || echo off)"
        echo "row:     $(grep "^$ROW_ID:" /etc/inittab 2>/dev/null || echo missing)"
        echo "running: $(label_of "$DATA/toonui" || true) $(md5 "$DATA/toonui")"
        echo "pinned:  $(label_of "$PIN/toonui" || true) $(cat "$PIN/toonui.md5" 2>/dev/null)"
        echo "bridge:  $(md5 "$DATA/quby_bridge") (pinned $(cat "$PIN/quby_bridge.md5" 2>/dev/null))"
        tail -n 8 "$LOG" 2>/dev/null
        ;;
    *)
        echo "usage: $0 setup|on|off|pin|status|remove|check|watch" >&2
        exit 1
        ;;
esac
