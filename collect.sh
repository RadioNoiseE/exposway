#!/usr/bin/env bash

STMPDIR="$HOME/.local/state/exposway"
LOGFILE="$STMPDIR/expose.log"

if [[ " $* " == *" -d "* ]]; then
    log() {
        local timestamp
        timestamp=$(date +"[%Y-%m-%d %H:%M:%S]")
        echo "$timestamp $1" >> "$LOGFILE"
    }
else
    log() {
        :
    }
fi


log "launched"

if [ ! -d "$STMPDIR" ]; then
    mkdir -p "$STMPDIR"
    if [ $? -ne 0 ]; then
        log "@ failed to open cache directory"
        exit 1
    fi
else
    find "$STMPDIR" -mindepth 1 -delete
    if [ $? -ne 0 ]; then
        log "@ failed to clear previous cache"
        exit 1
    fi
fi

cd "$STMPDIR" || exit 1
log "@ entered cache directory $STMPDIR"

swaymsg -t get_outputs | jq -r '.[] | select(.focused).rect | "\(.width) \(.height)"' >> output # geometry of the monitor
log "@ monitor geometry written (to output)"

swaymsg -mt subscribe '["window"]' |\
    while read -r win; do
        stat=$(jq -r '.change' <<< "$win")
        reference=$(jq -r '.container.name' <<< "$win")
        node=$(jq -r '.container.id' <<< "$win")
        focused=$(jq -r '.container.focused' <<< "$win")
        refresh=("focus" "fullscreen_mode" "move" "floating" "title") # when urgent or marked we take no measure, when newed the geometry hasn't yet been initialized
        delete=("close") # circumstance when snapshot needs to be destroyed
        if [[ "$reference" =~ "Expose Sway" ]]; then
            log "# expose!"
        elif [[ "${refresh[*]}" =~ "$stat" && "$focused" = "true" ]]; then
            log "* node $node info"
            log "  refresh: $stat"
            geometry=$(jq -j '.container.rect | "\(.x),\(.y) \(.width)x\(.height)"' <<< "$win")
            echo "$geometry $reference" > "$node"
            log "  geometry: $geometry"
            log "  reference: $reference"
            grim -g "$geometry" "${node}.png"
        elif [[ "${delete[*]}" =~ "$stat" ]]; then
            rm "$node" "${node}.png"
            log "! node $node destroyed"
        else
            continue
        fi
    done
