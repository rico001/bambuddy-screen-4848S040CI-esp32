#!/usr/bin/env bash
#
# Holt ein Bildschirmfoto vom Display ueber USB.
#
#   ./screenshot.sh                  automatisch gefundener Port
#   ./screenshot.sh /dev/cu.usbmodem1101
#
# Ablauf: Das Skript schickt das Wort "screenshot" auf die serielle Leitung,
# das Geraet nimmt den Bildschirm auf und gibt ihn als Base64 zwischen zwei
# Marken aus. Hier wird nur gelesen, was zwischen den Marken steht — ein
# mitlaufendes Log stoert also nicht.
#
# Ergebnis: screenshots/bildschirm-JJJJmmtt-HHMMSS.png
#
# Wichtig: Der Port ist exklusiv. Ein offener "pio device monitor" muss
# vorher beendet werden, sonst findet das Skript keinen freien Port.
#
# Mit bash ausfuehren, egal wie es aufgerufen wurde.
#
# Das Skript benutzt bash-Syntax (Arrays, arithmetische Schleife). Ein Aufruf
# als "sh screenshot.sh" landet auf manchen Systemen in einer anderen Shell,
# und dann scheitert es an einer Zeile, die mit dem eigentlichen Problem
# nichts zu tun hat.
if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

set -euo pipefail

# Der Mitschnitt ist eine rohe Bytefolge: Log-Zeilen mit Umlauten, dazu beim
# Start das Gebrabbel des Bootloaders in anderer Baudrate. Unter einer
# UTF-8-Locale steigen sed und grep darueber aus ("illegal byte sequence"),
# unter C sind es einfach Bytes.
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$ROOT/screenshots"
BAUD=115200
TIMEOUT=90

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    # Der S3 meldet sich je nach Kabel und Reset als usbmodem oder usbserial.
    PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)"
fi

if [[ -z "$PORT" || ! -e "$PORT" ]]; then
    echo "Kein serieller Port gefunden. Port als Argument angeben:" >&2
    echo "  ./screenshot.sh /dev/cu.usbmodem1101" >&2
    exit 1
fi

# Port oeffnen, BEVOR die Leitungsparameter gesetzt werden — und offen halten.
#
# Auf macOS verwirft der Treiber die Einstellungen, sobald der letzte
# Benutzer den Port schliesst; das naechste Oeffnen faengt wieder bei 9600
# Baud an. Genau das passierte hier: stty setzte 115200, dann oeffnete "cat"
# den Port erneut, und was ankam, war Zeichensalat.
#
# Ein einziger, offen gehaltener Lese-/Schreibkanal (fd 3) loest beides: Die
# Parameter bleiben gueltig, und der Adapter zieht nicht bei jedem Zugriff
# erneut die Reset-Leitung.
exec 3<>"$PORT"

# stty heisst auf macOS -f und auf Linux -F.
if stty -f "$PORT" >/dev/null 2>&1; then STTY=(stty -f "$PORT"); else STTY=(stty -F "$PORT"); fi

# clocal: keine Modemsteuerung erwarten. Ohne das wartet der Treiber bei
# manchen Adaptern auf ein Traegersignal, das nie kommt.
"${STTY[@]}" "$BAUD" raw -echo clocal -crtscts 2>/dev/null ||
    "${STTY[@]}" "$BAUD" raw -echo

RAW="$(mktemp)"
trap 'rm -f "$RAW"' EXIT

echo "==> Port $PORT"

# Erst den Mitschnitt starten, dann den Befehl schicken: Andersherum waere
# die Antwort schneller da als der Zuhoerer.
cat <&3 > "$RAW" &
READER=$!
trap 'kill "$READER" 2>/dev/null || true; exec 3>&- 3<&- 2>/dev/null || true; rm -f "$RAW"' EXIT

echo "==> Warte auf das Geraet ..."
sleep 3

# Wiederholt schicken statt einmal: Trifft der erste Versuch noch in den
# Startvorgang, fangen die naechsten ihn auf.
echo "==> Aufnahme angefordert, warte auf Daten ..."
for ((i = 0; i < TIMEOUT * 2; i++)); do
    if ((i % 6 == 0)); then
        printf 'screenshot\n' >&3 || true
    fi
    grep -qa "BB_SHOT_END" "$RAW" 2>/dev/null && break
    sleep 0.5
done

kill "$READER" 2>/dev/null || true
wait "$READER" 2>/dev/null || true

if ! grep -qa "BB_SHOT_END" "$RAW"; then
    echo >&2
    # Die drei Faelle unterscheiden sich darin, was ueberhaupt ankam. Das
    # gleich zu sagen erspart die Sucherei am falschen Ende.
    if [[ ! -s "$RAW" ]]; then
        echo "Vom Port kam gar nichts." >&2
        echo "  - Laeuft noch ein 'pio device monitor' auf $PORT?" >&2
        echo "  - Haengt das Geraet an einem Datenkabel, nicht nur an Strom?" >&2
    elif grep -qa "BB_SHOT_BEGIN" "$RAW"; then
        echo "Die Uebertragung brach mittendrin ab." >&2
        echo "  Mehr Zeit geben: TIMEOUT im Skript erhoehen." >&2
    else
        echo "Das Geraet meldet sich, kennt den Befehl aber nicht." >&2
        echo "  Die Firmware mit dem Screenshot-Befehl ist noch nicht drauf:" >&2
        echo "      pio run --target upload" >&2
        echo "  Letzte Zeilen vom Geraet:" >&2
        tail -3 "$RAW" | tr -c '[:print:]\n' '.' | sed 's/^/      /' >&2
    fi
    exit 1
fi

HEADER="$(grep -am1 "BB_SHOT_BEGIN" "$RAW")"
WIDTH="$(awk '{print $2}' <<<"$HEADER")"
HEIGHT="$(awk '{print $3}' <<<"$HEADER")"
echo "==> ${WIDTH}x${HEIGHT} empfangen"

mkdir -p "$OUT_DIR"
STAMP="$(date '+%Y%m%d-%H%M%S')"
TARGET="$OUT_DIR/bildschirm-$STAMP.png"

# Base64 zwischen den Marken herausschneiden, dekodieren und aus den
# RGB565-Rohdaten eine PNG bauen.
# Zwischen den Marken stehen nicht nur Bilddaten: Der Netzwerk-Task und MQTT
# schreiben waehrend der Uebertragung weiter ins Log. Deshalb wird nicht
# blind alles genommen, was dazwischen liegt, sondern nur Zeilen, die
# ausschliesslich aus Base64-Zeichen bestehen. Eine Log-Zeile enthaelt
# Leerzeichen oder Klammern und faellt damit heraus.
sed -n '/BB_SHOT_BEGIN/,/BB_SHOT_END/p' "$RAW" |
    tr -d '\r' |
    grep -E '^[A-Za-z0-9+/]+={0,2}$' |
    base64 -d > "$RAW.bin" 2>/dev/null || true

GOT=$(wc -c < "$RAW.bin" | tr -d ' ')
WANT=$((WIDTH * HEIGHT * 2))

if [[ "$GOT" -lt "$WANT" ]]; then
    echo "Nur $GOT von $WANT Bytes lesbar." >&2
    echo "  Kamen Zeichen abhanden, hilft eine hoehere Baudrate meist nicht —" >&2
    echo "  eher eine niedrigere. Sonst: Kabel oder Adapter." >&2
    exit 1
fi

python3 - "$RAW.bin" "$TARGET" "$WIDTH" "$HEIGHT" <<'PY'
import struct, sys, zlib

raw, target, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(raw, 'rb').read()
need = w * h * 2
if len(data) < need:
    sys.exit(f"Zu wenig Daten: {len(data)} statt {need} Bytes")

# RGB565 (little endian) auf 8 Bit je Kanal spreizen. Das schlichte
# Linksschieben liesse Weiss bei 0xF8 enden — die oberen Bits werden deshalb
# unten wieder eingesetzt.
rows = bytearray()
pos = 0
for y in range(h):
    rows.append(0)  # PNG-Filter "None" je Zeile
    for x in range(w):
        px = data[pos] | (data[pos + 1] << 8)
        pos += 2
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

def chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload +
            struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(rows), 9))
png += chunk(b'IEND', b'')
open(target, 'wb').write(png)
PY

rm -f "$RAW.bin"
echo "==> $TARGET"

command -v open >/dev/null && open "$TARGET" || true
