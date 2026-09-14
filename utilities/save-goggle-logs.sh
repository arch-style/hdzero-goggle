#!/bin/sh
#
# Copy goggle application logs into the local archive.
#
#   utilities/save-goggle-logs.sh [source] [destination]
#
# source        an SD card or any directory holding HDZGOGGLE*.log files
#               (default /Volumes/NO NAME), or a single log file -- somebody
#               else's, sent by mail. Its name becomes part of the archive
#               name, so rename it to something that says whose it is first.
# destination   default logs/ in the checkout, which is not tracked.
#
# The goggles keep the current boot as HDZGOGGLE.log and the finished ones in
# boot-logs/, numbered upwards, up to 999 of them. That is a lot of room to
# notice something and go and get the card, but it is still a window: the
# oldest falls off, and this is what takes things out of the queue.
#
# Every candidate is read to be checksummed, so importing a card holding
# hundreds of logs takes a moment. Ones already in the archive are then
# skipped, so it is only ever slow, never wasteful.
#
# A log already in the archive is recognised by its checksum and skipped, so
# running this on the same card twice costs nothing, and running it every time
# the card is in the machine is the point -- the boot worth keeping is always
# identified after the fact.
#
# Names are <archive date>-[label-]boot<total>ms-<checksum>.log.gz. The date is
# this machine's: the goggles have no RTC battery and restore the same fake
# time every boot, so their own timestamps sort nothing. The boot total is read
# out of the log, so the boot that took ten seconds can be found by name.

set -e

SRC=${1:-/Volumes/NO NAME}
DEST=${2:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/logs}

# Held in the positional parameters rather than a string, so a card mounted at
# "/Volumes/NO NAME" survives. A glob that matches nothing stays literal and
# fails the -s test below, which is the wanted answer either way.
if [ -f "$SRC" ]; then
    set -- "$SRC"
    label=$(basename "$SRC" | sed 's/\.log$//; s/[^A-Za-z0-9._-]/_/g')-
elif [ -d "$SRC" ]; then
    set -- "$SRC/HDZGOGGLE.log" "$SRC"/boot-logs/HDZGOGGLE.*.log \
           "$SRC/HDZGOGGLE.prev.log" "$SRC/RECORD.log" "$SRC"/boot-logs/RECORD.*.log
    label=
else
    echo "no card, directory or file at $SRC" >&2
    exit 1
fi

mkdir -p "$DEST"

saved=0
skipped=0
seen=0

for src do
    [ -s "$src" ] || continue
    seen=$((seen + 1))

    sum=$(shasum -a 1 "$src" | cut -c1-8)

    if ls "$DEST"/*-"$sum".log.gz >/dev/null 2>&1; then
        skipped=$((skipped + 1))
        continue
    fi

    boot=$(grep -o 'boot total: app start to switch done [0-9]*ms' "$src" |
           tail -1 | grep -o '[0-9]*' || true)
    [ -n "$boot" ] || boot=unknown

    # The record process's own output. rc.sh on the goggles sends it to
    # /mnt/extsd/RECORD.log whenever that file exists, one boot at a time, so
    # an empty RECORD.log on the card is how you ask for it. It has no boot
    # total; the name carries "record" instead.
    case $(basename "$src") in
    RECORD.log) name="record-$sum" ;;
    RECORD.*.log) name="record$(basename "$src" | sed 's/RECORD\.\([0-9]*\)\.log/\1/')-$sum" ;;
    *) name="${label}boot${boot}ms-$sum" ;;
    esac

    # Written by the app as its first line. Missing on logs from before that,
    # and on logs from stock firmware.
    build=$(grep -m1 -o 'build: .*' "$src" || echo "build: not stated")

    out="$DEST/$(date +%Y-%m-%d_%H%M)-$name.log.gz"
    gzip -c "$src" > "$out"
    saved=$((saved + 1))
    echo "saved $(basename "$out")"
    echo "      $(wc -c < "$src" | tr -d ' ') bytes from $(basename "$src"), $build"
done

if [ "$seen" = 0 ]; then
    echo "no HDZGOGGLE logs in $SRC" >&2
    exit 1
fi

echo "$saved saved, $skipped already had"
