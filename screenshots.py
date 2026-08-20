#!/usr/bin/env python3
"""Bildschirmfotos fuer den README vom laufenden Geraet holen.

    ./screenshots.py                 die vier Kacheln: AMS, Status, Archiv, System
    ./screenshots.py now             das aktuelle Bild, mit Zeitstempel im Namen
    ./screenshots.py jog             dasselbe als docs/screen-jog.png
    ./screenshots.py --host 192.168.178.42

Das Geraet muss dafuer erreichbar sein und der Web-Update-Schalter an:
Einstellungen -> FIRMWARE -> Web-Update. Die Adresse steht in der Zeile
darunter.

Vier der Bilder holt das Skript allein, weil es die Kacheln ueber die Webseite
umschalten kann. Die uebrigen -- Jog, Einstellungen, Smart Plugs,
Bildschirmschoner, Dialog -- liegen als Vollbild ueber den Kacheln oder haengen
an einem Zustand des Druckers; dorthin muss jemand am Geraet navigieren. Dafuer
ist die zweite Aufrufform da: hinsteuern, dann "now" uebergeben. Das legt
screen-now-JJJJmmtt-HHMMSS.png an, ueberschreibt also nichts -- wer den Treffer
behalten will, benennt ihn hinterher auf den Namen im README um.

Das Geraet liefert BMP (RGB565, unkomprimiert). Umgerechnet wird hier, damit
auf dem Board kein Deflate laufen muss -- 450 KB Bilddaten sind fuer den
kleinen Heap dort das groessere Problem als fuer diesen Rechner.
"""

import argparse
import datetime
import struct
import sys
import urllib.error
import urllib.request
import zlib
from pathlib import Path

# Die Adresse des Displays im Heimnetz. Ueberschreibbar mit --host, weil sie
# sich nach einem Neustart des Routers aendern kann.
DEFAULT_HOST = "192.168.178.95"

DOCS = Path(__file__).resolve().parent / "docs"

# Kachel-Index im Geraet -> Bilddatei. Die Reihenfolge ist die der
# Navigationsleiste (siehe tile_at() in src/main.cpp). Kachel 2 (Auftraege)
# fehlt hier, weil der README kein Bild davon zeigt.
TILES = {
    0: "ams",
    1: "status",
    3: "archiv",
    4: "system",
}

# Was nur von Hand zu erreichen ist. Die Liste dient bloss der Fehlermeldung,
# uebergeben werden darf jeder Name.
BY_HAND = ["jog", "einstellungen", "smart-plugs", "bildschirmschoner", "dialog"]

# Das Wort fuer "einfach jetzt, egal was da steht". Bekommt die Uhrzeit
# angehaengt, damit mehrere Versuche nebeneinander liegen bleiben.
NOW = "now"

TIMEOUT = 20


def fail(msg):
    print(msg, file=sys.stderr)
    sys.exit(1)


def request(host, path, method="GET"):
    url = "http://%s%s" % (host, path)
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return resp.read()
    except urllib.error.HTTPError as e:
        # Das Geraet schickt Klartext mit -- den weiterzureichen erspart das
        # Raten, ob es am Speicher, am Pfad oder am Zustand lag.
        detail = e.read().decode("utf-8", "replace").strip()
        fail("%s antwortete mit %d: %s" % (url, e.code, detail or e.reason))
    except urllib.error.URLError as e:
        fail("%s ist nicht erreichbar (%s).\n"
             "  - Laeuft der Web-Update-Dienst? Einstellungen -> FIRMWARE\n"
             "  - Stimmt die Adresse? Sonst mit --host angeben." % (url, e.reason))


def bmp_to_png(bmp):
    """Das 16-Bit-BMP des Geraets in eine PNG umrechnen.

    Der Kopf wird gelesen statt angenommen: Aendert sich auf dem Geraet etwas
    an Groesse oder Aufbau, soll hier eine Meldung stehen und kein still
    verschobenes Bild herauskommen.
    """
    if len(bmp) < 66 or bmp[:2] != b"BM":
        fail("Das Geraet hat kein BMP geliefert.")

    offset, = struct.unpack_from("<I", bmp, 10)
    width, height = struct.unpack_from("<ii", bmp, 18)
    bpp, = struct.unpack_from("<H", bmp, 28)
    compression, = struct.unpack_from("<I", bmp, 30)

    if bpp != 16 or compression != 3:
        fail("Unerwartetes BMP: %d Bit, Kompression %d — erwartet 16 und 3."
             % (bpp, compression))

    stride = width * 2
    need = offset + stride * abs(height)
    if len(bmp) < need:
        fail("BMP ist unvollstaendig: %d von %d Bytes." % (len(bmp), need))

    # Positive Hoehe heisst: unterste Zeile zuerst. PNG will es andersherum.
    rows = range(abs(height) - 1, -1, -1) if height > 0 else range(abs(height))

    out = bytearray()
    for y in rows:
        line = bmp[offset + y * stride:offset + (y + 1) * stride]
        out.append(0)  # PNG-Filter "None"
        for x in range(0, stride, 2):
            px = line[x] | (line[x + 1] << 8)
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            # Die oberen Bits unten wieder einsetzen, sonst endet Weiss bei
            # 0xF8 und das ganze Bild wirkt grau.
            out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4),
                          (b << 3) | (b >> 2)))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, abs(height), 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(out), 9))
    png += chunk(b"IEND", b"")
    return png


def grab(host, name):
    """Aufnehmen und unter docs/screen-<name>.png ablegen."""
    if name == NOW:
        name = "%s-%s" % (NOW, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))

    bmp = request(host, "/screenshot.bmp")
    png = bmp_to_png(bmp)

    target = DOCS / ("screen-%s.png" % name)
    before = target.stat().st_size if target.exists() else 0
    target.write_bytes(png)

    note = "neu" if before == 0 else "war %d KB" % (before // 1024)
    print("    %-28s %6d KB  (%s)" % (target.name, len(png) // 1024, note))


def main():
    ap = argparse.ArgumentParser(
        description="Bildschirmfotos vom Geraet in docs/ ablegen.")
    ap.add_argument("names", nargs="*",
                    help="\"now\" nimmt auf, was gerade auf dem Display steht, "
                         "und haengt die Uhrzeit an den Namen. Ein fester Name "
                         "wie \"jog\" schreibt stattdessen docs/screen-jog.png. "
                         "Ohne Angabe werden die vier Kacheln durchgeschaltet.")
    ap.add_argument("--host", default=DEFAULT_HOST,
                    help="Adresse des Displays (Standard: %s)" % DEFAULT_HOST)
    args = ap.parse_args()

    if not DOCS.is_dir():
        fail("%s gibt es nicht — vom Repo-Verzeichnis aus aufrufen." % DOCS)

    if args.names:
        print("==> %s: nimmt auf, was gerade auf dem Display steht" % args.host)
        for name in args.names:
            name = name.removeprefix("screen-").removesuffix(".png")
            if name in TILES.values():
                print("    Hinweis: %s liegt auf einer Kachel und ginge auch "
                      "ohne Argument." % name)
            elif name != NOW and name not in BY_HAND:
                print("    Hinweis: %s ist im README nicht eingebunden." % name)
            grab(args.host, name)
        return

    print("==> %s: schaltet die Kacheln durch" % args.host)
    for index, name in sorted(TILES.items()):
        # Das Geraet wartet nach dem Umschalten selbst, bis die Kachel steht
        # (handle_tile in src/ota_service.cpp) — hier braucht es keine zweite
        # Wartezeit.
        request(args.host, "/tile?i=%d" % index, method="POST")
        grab(args.host, name)

    print("\nDie uebrigen Bilder gehen nur von Hand: am Geraet hinsteuern,")
    print("dann ./screenshots.py now")


if __name__ == "__main__":
    main()
