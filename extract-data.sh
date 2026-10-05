#!/usr/bin/env bash
# Extract dbc, cameras, maps, vmaps and mmaps from the 3.3.5a (12340) client into the server data directory,
# and point worldserver.conf's DataDir at it.
#
# Custom MPQs (anything that isn't a stock 3.3.5a archive, e.g. Patch-W.mpq or patch-enUS-4.mpq) are moved out of
# the client's Data folder while the tools run, so only Blizzard data is extracted. They're always moved back
# afterwards, also when a step fails or the script is interrupted.
#
# Usage: ./extract-data.sh [all|maps|vmaps|mmaps|restore-mpqs]...   (default: all)
#   restore-mpqs  only puts back MPQs left behind by a run that was killed before it could clean up
#
# Override paths with env vars: CLIENT_DIR, SERVER_DIR, THREADS
set -euo pipefail

TC_CODE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_DIR="${CLIENT_DIR:-$HOME/Games/World of Warcraft}"   # folder containing Wow.exe and Data/
SERVER_DIR="${SERVER_DIR:-$TC_CODE_DIR/server}"
THREADS="${THREADS:-$(nproc)}"

BIN="$SERVER_DIR/bin"
DATA_DIR="$SERVER_DIR/data"
WORLD_CONF="$SERVER_DIR/etc/worldserver.conf"
# Inside the client folder so moving files is an instant rename on the same filesystem.
STASH_DIR="$CLIENT_DIR/_custom_mpqs_disabled"

die() { echo "error: $*" >&2; exit 1; }
step() { echo; echo "=== $* ($(date +%T)) ==="; }

# Stock 3.3.5a archives, lowercase. {locale} is replaced with the locale folder's name (enUS, deDE, ...).
STOCK_MPQS=(
    common.mpq common-2.mpq expansion.mpq lichking.mpq patch.mpq patch-2.mpq patch-3.mpq
    "{locale}/backup-{locale}.mpq" "{locale}/base-{locale}.mpq"
    "{locale}/locale-{locale}.mpq" "{locale}/speech-{locale}.mpq"
    "{locale}/expansion-locale-{locale}.mpq" "{locale}/expansion-speech-{locale}.mpq"
    "{locale}/lichking-locale-{locale}.mpq" "{locale}/lichking-speech-{locale}.mpq"
    "{locale}/patch-{locale}.mpq" "{locale}/patch-{locale}-2.mpq" "{locale}/patch-{locale}-3.mpq"
)

# True if the path (relative to Data/) is a stock archive.
is_stock_mpq() {
    local rel="${1,,}" locale="" stock
    [[ "$rel" == */* ]] && locale="${rel%%/*}"
    for stock in "${STOCK_MPQS[@]}"; do
        if [[ "$stock" == *"{locale}"* ]]; then
            [[ -n "$locale" && "$rel" == "${stock//\{locale\}/$locale}" ]] && return 0
        elif [[ "$rel" == "$stock" ]]; then
            return 0
        fi
    done
    return 1
}

hide_custom_mpqs() {
    step "Moving custom MPQs out of the client's Data folder"
    local file rel moved=0
    while IFS= read -r -d '' file; do
        rel="${file#"$CLIENT_DIR/Data/"}"
        is_stock_mpq "$rel" && continue
        mkdir -p "$STASH_DIR/$(dirname "$rel")"
        mv -n "$file" "$STASH_DIR/$rel"
        echo "   hid Data/$rel"
        moved=$((moved + 1))
    done < <(find "$CLIENT_DIR/Data" -maxdepth 2 -type f -iname '*.mpq' -print0)
    (( moved )) || echo "   none found"
}

restore_custom_mpqs() {
    [[ -d "$STASH_DIR" ]] || return 0
    step "Moving custom MPQs back into the client's Data folder"
    local file rel
    while IFS= read -r -d '' file; do
        rel="${file#"$STASH_DIR/"}"
        if [[ -e "$CLIENT_DIR/Data/$rel" ]]; then
            echo "!! Data/$rel already exists, leaving the hidden copy in $STASH_DIR" >&2
            continue
        fi
        mkdir -p "$CLIENT_DIR/Data/$(dirname "$rel")"
        mv "$file" "$CLIENT_DIR/Data/$rel"
        echo "   restored Data/$rel"
    done < <(find "$STASH_DIR" -type f -print0)
    find "$STASH_DIR" -depth -type d -empty -delete
}

extract_maps() {
    step "Extracting dbc, cameras and maps"
    "$BIN/mapextractor" -i "$CLIENT_DIR" -o "$DATA_DIR"
}

extract_vmaps() {
    step "Extracting vmaps"
    rm -rf Buildings vmaps   # vmap4extractor refuses to run with an old Buildings folder
    "$BIN/vmap4extractor" -d "$CLIENT_DIR"
    mkdir -p vmaps
    "$BIN/vmap4assembler" Buildings vmaps
    rm -rf Buildings
}

extract_mmaps() {
    [[ -d maps && -d vmaps && -d dbc ]] || die "mmaps need dbc, maps and vmaps extracted first"
    step "Generating mmaps (this takes a long time)"
    rm -rf mmaps
    "$BIN/mmaps_generator" --threads "$THREADS"
}

set_data_dir() {
    [[ -f "$WORLD_CONF" ]] || { echo "!! $WORLD_CONF not found, set DataDir to \"$DATA_DIR\" yourself"; return; }
    sed -i -E "s|^(\s*DataDir\s*=\s*).*|\1\"$DATA_DIR\"|" "$WORLD_CONF"
    echo ">> DataDir in worldserver.conf set to \"$DATA_DIR\""
}

[[ -f "$CLIENT_DIR/Wow.exe" && -d "$CLIENT_DIR/Data" ]] || die "no 3.3.5a client (Wow.exe + Data/) in $CLIENT_DIR"

targets=("${@:-all}")
if [[ "${targets[*]}" == "restore-mpqs" ]]; then
    restore_custom_mpqs
    exit 0
fi

for t in mapextractor vmap4extractor vmap4assembler mmaps_generator; do
    [[ -x "$BIN/$t" ]] || die "missing $BIN/$t (run ./rebuild.sh first)"
done
for t in "${targets[@]}"; do
    case "$t" in
        all|maps|vmaps|mmaps) ;;
        *) die "unknown target '$t' (use all, maps, vmaps, mmaps or restore-mpqs)" ;;
    esac
done

# A previous run that was killed (kill -9, power loss) can leave MPQs hidden; put them back before starting.
restore_custom_mpqs
trap restore_custom_mpqs EXIT
trap 'exit 130' INT TERM
hide_custom_mpqs

mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

for t in "${targets[@]}"; do
    case "$t" in
        all)   extract_maps; extract_vmaps; extract_mmaps ;;
        maps)  extract_maps ;;
        vmaps) extract_vmaps ;;
        mmaps) extract_mmaps ;;
    esac
done

set_data_dir

step "Done"
du -sh "$DATA_DIR"/* 2>/dev/null
