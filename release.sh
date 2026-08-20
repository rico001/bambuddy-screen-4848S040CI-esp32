#!/usr/bin/env bash
#
# Legt die gebaute Firmware unter ihrer Versionsnummer in firmware-build/ ab.
#
# Die Datei aus .pio/build heisst bei jedem Build gleich (firmware.bin) und
# wird beim naechsten ueberschrieben. Damit laesst sich hinterher nicht mehr
# sagen, was auf dem Geraet liegt — und beim Web-Update waehlt man im Browser
# aus einer Reihe identisch benannter Dateien. Deshalb bekommt jede Fassung
# hier einen eigenen Namen aus version.txt.
#
#   ./release.sh                     vorhandenen Build ablegen
#   ./release.sh --build             vorher "pio run" laufen lassen
#   ./release.sh --force             bestehende Datei derselben Version ersetzen
#   ./release.sh --upload 192.168.1.23   danach aufs Geraet schicken
#
# Fuer --upload muss am Display "Einstellungen -> FIRMWARE -> Web-Update"
# eingeschaltet sein; die dort angezeigte Adresse ist das Ziel.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_NAME="esp32-4848S040CIY1"
BUILD_BIN="$ROOT/.pio/build/$ENV_NAME/firmware.bin"
VERSION_FILE="$ROOT/version.txt"
OUT_DIR="$ROOT/firmware-build"

# Groesse einer OTA-Partition aus min_spiffs.csv (0x1E0000). Daran gemessen
# wird die Auslastung ausgegeben: Sobald das Abbild nicht mehr in den Slot
# passt, ist kein Web-Update mehr moeglich — das soll auffallen, bevor es
# soweit ist, und nicht erst am abgelehnten Upload.
SLOT_BYTES=$((0x1E0000))

do_build=0
force=0
upload_host=""

while [[ $# -gt 0 ]]; do
    case "$1" in
    --build) do_build=1 ;;
    --force) force=1 ;;
    --upload)
        # Adresse als eigenes Wort oder mit "=" — beides tippt sich jemand
        # aus dem Gedaechtnis so hin.
        if [[ $# -lt 2 || "$2" == -* ]]; then
            echo "--upload braucht eine Adresse, z.B. --upload 192.168.1.23" >&2
            exit 2
        fi
        upload_host="$2"
        shift
        ;;
    --upload=*) upload_host="${1#--upload=}" ;;
    -h | --help)
        sed -n '3,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "Unbekannte Option: $1" >&2
        exit 2
        ;;
    esac
    shift
done

# Ob mit oder ohne "http://" eingegeben: fuer curl braucht es das Schema, und
# niemand soll sich merken muessen, in welcher Form es erwartet wird.
upload_host="${upload_host#http://}"
upload_host="${upload_host#https://}"
upload_host="${upload_host%/}"

# --- Version lesen -------------------------------------------------------
if [[ ! -f "$VERSION_FILE" ]]; then
    echo "version.txt fehlt ($VERSION_FILE)" >&2
    exit 1
fi

# Erste nicht leere Zeile, ohne Kommentar und ohne Leerzeichen: so darf in
# der Datei auch eine Notiz stehen, ohne den Dateinamen zu verunstalten.
VERSION="$(grep -v '^[[:space:]]*#' "$VERSION_FILE" | grep -m1 '[^[:space:]]' | tr -d '[:space:]' || true)"

if [[ -z "$VERSION" ]]; then
    echo "version.txt enthaelt keine Version" >&2
    exit 1
fi

if [[ ! "$VERSION" =~ ^[A-Za-z0-9._+-]+$ ]]; then
    echo "Version '$VERSION' enthaelt Zeichen, die nicht in einen Dateinamen gehoeren" >&2
    exit 1
fi

# --- Bauen ---------------------------------------------------------------
if [[ $do_build -eq 1 ]]; then
    echo "==> pio run"
    (cd "$ROOT" && pio run)
fi

if [[ ! -f "$BUILD_BIN" ]]; then
    echo "Kein Build gefunden: $BUILD_BIN" >&2
    echo "Erst 'pio run' laufen lassen oder ./release.sh --build benutzen." >&2
    exit 1
fi

# --- Ist der Build noch aktuell? -----------------------------------------
# Eine alte firmware.bin unter neuer Versionsnummer abzulegen faellt erst auf,
# wenn die Aenderung auf dem Geraet fehlt — und dann sucht man den Fehler im
# Code statt im Ablauf.
newer="$(find "$ROOT/src" "$ROOT/include" "$ROOT/platformio.ini" \
    -newer "$BUILD_BIN" -type f -print -quit 2>/dev/null || true)"
if [[ -n "$newer" ]]; then
    echo "WARNUNG: $(basename "$newer") ist neuer als der Build." >&2
    echo "         Vermutlich fehlt ein 'pio run' (oder ./release.sh --build)." >&2
fi

# --- Ziel ----------------------------------------------------------------
mkdir -p "$OUT_DIR"
TARGET="$OUT_DIR/bambuddy-display-v$VERSION.bin"

unchanged=0
if [[ -e "$TARGET" && $force -ne 1 ]]; then
    if cmp -s "$BUILD_BIN" "$TARGET"; then
        # Kein Abbruch: Wer --upload angehaengt hat, will die Datei aufs
        # Geraet schicken — ob sie neu ist oder schon dalag, aendert daran
        # nichts.
        echo "Unveraendert: $(basename "$TARGET") liegt bereits vor."
        unchanged=1
    else
        echo "$(basename "$TARGET") existiert schon und hat einen anderen Inhalt." >&2
        echo "Version in version.txt erhoehen — oder mit --force ueberschreiben." >&2
        exit 1
    fi
fi

[[ $unchanged -eq 1 ]] || cp "$BUILD_BIN" "$TARGET"

# --- Auskunft ------------------------------------------------------------
size=$(wc -c <"$TARGET" | tr -d ' ')
pct=$((size * 100 / SLOT_BYTES))

# Die ersten acht Byte des ELF-Hashes. Er steht im App-Deskriptor ab Offset
# 0x20 des Abbilds, Feld app_elf_sha256 bei 0xB0, und wird beim Erzeugen der
# .bin eingetragen — er ist damit fuer jeden Build ein anderer.
#
# Nicht ausgelesen werden Datum und Version aus demselben Deskriptor: Die
# stammen aus Espressifs Uebersetzung der vorkompilierten IDF-Bibliothek
# ("Mar 5 2024") und haben mit diesem Build nichts zu tun. Der Zeitpunkt hier
# ist deshalb der der Datei, die Version die aus version.txt.
build_id="$(dd if="$TARGET" bs=1 skip=176 count=8 2>/dev/null | xxd -p)"
built_at="$(date -r "$BUILD_BIN" '+%d.%m.%Y, %H:%M')"

echo
echo "Abgelegt:      firmware-build/$(basename "$TARGET")"
echo "Version:       $VERSION"
echo "Groesse:       $((size / 1024)) KB von $((SLOT_BYTES / 1024)) KB ($pct % der OTA-Partition)"
echo "Gebaut:        $built_at"
echo "Build-Kennung: $build_id"
echo "SHA-256:       $(shasum -a 256 "$TARGET" | cut -d' ' -f1)"

if [[ -z "$upload_host" ]]; then
    echo
    echo "Diese Datei im Browser des Displays hochladen (Einstellungen -> FIRMWARE),"
    echo "oder gleich hier: ./release.sh --upload <adresse-aus-den-einstellungen>"
    exit 0
fi

# --- Hochladen -----------------------------------------------------------
#
# Dasselbe, was das Formular im Browser schickt: ein Multipart-POST auf
# /update. Der Feldname spielt keine Rolle, das Geraet nimmt den einzigen
# Teil der Anfrage — er heisst hier trotzdem "firmware", damit ein Blick in
# den Serial-Log dasselbe zeigt wie beim Weg ueber die Seite.
echo
echo "==> Upload nach http://$upload_host/update"

if ! curl --fail --show-error --progress-bar \
    --connect-timeout 5 --max-time 600 \
    -F "firmware=@$TARGET" \
    "http://$upload_host/update"; then
    echo >&2
    echo "Upload fehlgeschlagen." >&2
    echo "Laeuft das Web-Update? Einstellungen -> FIRMWARE, dort steht die Adresse." >&2
    exit 1
fi
echo

# --- Nachsehen, was jetzt laeuft -----------------------------------------
#
# Das Geraet startet nach dem letzten Byte neu und braucht ein paar Sekunden,
# bis Netzwerk und Webserver wieder stehen. Die Build-Kennung von der Seite
# gegen die der hochgeladenen Datei zu halten ist die einzige Antwort auf die
# Frage, die nach jedem Update kommt: laeuft das neue Abbild wirklich?
echo "==> Warte auf den Neustart"

for _ in $(seq 1 20); do
    sleep 3
    page="$(curl -s --connect-timeout 3 --max-time 5 "http://$upload_host/" || true)"
    [[ -z "$page" ]] && continue

    running="$(printf '%s' "$page" |
        sed -n 's/.*Build-Kennung<\/td><td>\([0-9a-f]*\)<.*/\1/p' | head -1)"
    [[ -z "$running" ]] && continue

    if [[ "$running" == "$build_id" ]]; then
        echo "Laeuft: $build_id — das ist die soeben hochgeladene Fassung."
        exit 0
    fi

    echo "Das Geraet meldet Build $running, erwartet war $build_id." >&2
    echo "Der Upload kam an, aber gebootet wurde etwas anderes." >&2
    exit 1
done

echo "Das Geraet hat sich innerhalb einer Minute nicht zurueckgemeldet." >&2
echo "Der Upload war erfolgreich; sieh am Display nach, ob es neu gestartet ist." >&2
exit 1
