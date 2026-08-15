# Projektnotizen

Display für eine selbst gehostete [Bambuddy](https://wiki.bambuddy.cool)-Instanz
auf einem Sunton ESP32-4848S040CI (480x480, LVGL 9.2).

**Getestet gegen Bambuddy-Version: v1.2.5.3**
**Letzte Prüfung: 2026-08-15, 23:16 Uhr — Instanz auf v1.2.5.3, alle
genutzten Endpunkte, Parameter und Antwortfelder unverändert; nur die
Versionsangaben nachgezogen, kein Code angepasst**

Dieselbe Versionsnummer steht ein zweites Mal im Code, als
`BB_TESTED_VERSION` in `src/bambuddy_version.h`. Das Display fragt die
laufende Fassung selbst ab und vergleicht sie damit: Der System-Screen zeigt
oben rechts ein grünes Ausrufezeichen, wenn beide übereinstimmen, sonst ein
rotes. **Beide Stellen zusammen ändern** — sonst behauptet das Gerät eine
Abweichung, die es nicht gibt, oder verschweigt eine, die es gibt.

---

## Update-Prompt

> Prüfe, ob dieses Projekt noch zur laufenden Bambuddy-Version passt, und
> passe es an, wo nötig.

Ablauf:

**1. Version feststellen.**

```bash
curl -s -H "X-API-Key: $KEY" {BASE_URL}/api/v1/updates/check
```

```jsonc
{
  "update_available": false,
  "current_version": "1.2.5.2",   // <- DIESE laeuft im Smarthome: massgeblich
  "latest_version":  "1.2.5.2",   //    nur auf GitHub verfuegbar: ignorieren
  ...
}
```

**Verglichen wird ausschliesslich gegen `current_version`.** Nur diese
Fassung laeuft auf der Instanz, und nur ihre API bedient das Display.
`latest_version` sagt lediglich, ob Rico seine Instanz aktualisieren
koennte — fuer diesen Abgleich ist der Wert bedeutungslos.

Ist `update_available` wahr, das erwaehnen (die Instanz hinkt hinterher),
aber **nicht** gegen `latest_version` anpassen: Code gegen eine Version zu
schreiben, die nirgends laeuft, macht das Display sofort kaputt.

Ohne API-Key geht ersatzweise `GET /api/v1/updates/version` — liefert
ebenfalls die **laufende** Fassung (laut Beschreibung „Get current
application version"), nicht die verfuegbare.

`BASE_URL` steht in `include/secrets.h` (`BAMBUDDY_DEFAULT_URL`) oder auf
dem Gerät unter Einstellungen → Bambuddy; ist die lokale Adresse von hier
aus nicht erreichbar, tut es die Tunnel-Adresse. Stimmt `current_version`
mit der oben genannten überein, ist nichts zu tun — dann das melden und
aufhören.

**2. Aktuelle API-Beschreibung holen.**

```bash
curl -s {BASE_URL}/openapi.json -o bambuddy-docs/api/api-docs.json
```

Die interaktive Fassung liegt unter `{BASE_URL}/docs`. Die lokale Kopie in
`bambuddy-docs/api/api-docs.json` ist der Stand, gegen den dieser Code
entwickelt wurde — sie wird mit diesem Schritt aktualisiert.

**3. Endpunkte, Parameter und Felder prüfen.**
Es genügt nicht, dass ein Pfad noch existiert — er muss sich **genauso
benutzen lassen wie bisher**. Deshalb alle drei Ebenen abgleichen:

```bash
grep -rn 'bambuddy_url("' src/*.cpp        # aufgerufene Endpunkte
grep -rn 'filter\[' src/*.cpp              # ausgewertete Antwortfelder
```

- **Endpunkt:** Pfad und HTTP-Methode noch vorhanden?
- **Parameter:** Heißen die mitgeschickten Query-Parameter noch gleich, sind
  sie weiterhin erlaubt, und ist keiner neu als Pflicht dazugekommen?
  Ebenso die Rumpffelder bei `POST /queue/`
  (`printer_id`, `archive_id`, `manual_start`).
- **Antwortfelder:** Existieren alle Felder aus den `JsonDocument
  filter`-Blöcken noch im zugehörigen Schema (`PrinterStatus`,
  `PrintQueueItemResponse`, `ArchiveResponse`, Smart-Plug-Antwort)?

Der letzte Punkt ist der heimtückische: Ein umbenanntes oder entferntes
Antwortfeld erzeugt keinen Fehler, sondern stillschweigend eine Null. Auf
dem Display steht dann dauerhaft „0 %" oder „Unbekannt", und niemand weiß,
warum. Deshalb Feld für Feld gegen das Schema prüfen, nicht nur den Pfad.

Ändert sich etwas an Bedeutung oder Einheit eines Feldes (etwa Sekunden
statt Minuten), fällt das in keinem Schema auf — bei verdächtigen Werten
gegen die Anzeige des Druckers gegenprüfen.

**4. Abweichungen im Code nachziehen.**
Endpunkte, Parameter und Feldnamen anpassen. Bei entfernten Endpunkten nach
dem vorgesehenen Ersatz suchen: Bambuddy liefert ihn oft in der
Fehlerantwort mit (siehe unten).

**5. Kopf dieser Datei nachführen — immer, auch wenn nichts zu tun war.**

```markdown
**Getestet gegen Bambuddy-Version: v1.2.5.2**
**Letzte Prüfung: 2026-08-08, 14:02 Uhr — unverändert**
```

- `Getestet gegen …` auf `current_version` setzen, sobald ein Durchlauf diese
  Fassung tatsächlich durchgeprüft hat — auch wenn dabei kein Code angepasst
  werden musste. Die Zeile sagt aus, gegen welche Version geprüft wurde, nicht
  wann zuletzt etwas kaputt war; bliebe sie stehen, behauptete der
  System-Screen eine Abweichung, die die Prüfung gerade widerlegt hat.
- `Letzte Prüfung` **bei jedem Durchlauf** auf Datum **und Uhrzeit** setzen
  (`date '+%Y-%m-%d, %H:%M'`), dahinter das Ergebnis: `unverändert` oder eine
  knappe Zusammenfassung der Anpassungen.
- Wird `Getestet gegen …` geändert, **alle drei weiteren Stellen im selben Zug
  mitziehen** — sonst widersprechen sie einander:
  1. `BB_TESTED_VERSION` in `src/bambuddy_version.h` (daran hängt das
     Ausrufezeichen im System-Screen),
  2. die Überschrift `## Genutzte Endpunkte (Stand vX.Y.Z)` weiter unten,
  3. `## Tested With` in `README.md` — das ist die einzige Versionsangabe
     ausserhalb dieser Datei und des Codes und wird deshalb am leichtesten
     vergessen. Zu finden mit `grep -rn '1\.2\.5' README.md CLAUDE.md src/`.

Ohne diesen Zeitstempel weiß beim nächsten Mal niemand, ob die Prüfung von
gestern oder von vor einem halben Jahr stammt — und damit auch nicht, wie
viel Vertrauen die Aussage „passt noch" verdient.

---

## Was die API bisher verlangt hat

Erfahrungen aus vorangegangenen Anpassungen — beim nächsten Update zuerst
hier nachsehen:

- **Fehlerantworten enthalten `{"detail": "..."}` mit Klartext.** Als
  `/archives/{id}/reprint` entfernt wurde, stand die Lösung wörtlich darin:
  *„Direct archive reprint has been removed. Create a print queue item with
  POST /queue/."* Der Text wird deshalb ausgelesen und in der Oberfläche
  angezeigt (`read_error_detail()` in `bambuddy_archive.cpp`) — bei neuen
  Modulen genauso halten.
- **Archivdruck läuft zweistufig:** `POST /queue/` mit
  `{"printer_id":N,"archive_id":M,"manual_start":true}` anlegen, dann
  `POST /queue/{id}/start`. `manual_start` verhindert, dass der Auftrag
  losläuft, bevor die Rückfrage zur Druckplatte beantwortet ist.
- **Bilder brauchen den Kamera-Token** in der URL, nicht den API-Key:
  `/printers/{id}/cover`, `/archives/{id}/thumbnail`,
  `/printers/{id}/camera/snapshot`.
- **Wartet der Drucker auf `awaiting_plate_clear`**, wird jeder Start
  abgelehnt. Vorher `POST /printers/{id}/clear-plate` schicken.
- **Einheiten sind nicht dokumentiert.** `print_time_seconds` in der
  Warteschlange ist in Sekunden, `remaining_time` im Druckerstatus wird als
  Minuten interpretiert — beim nächsten echten Druck gegen die Anzeige des
  Druckers prüfen (der Rohwert steht im Serial-Log).
- **Beim AMS-Slot rechnet das Frontend mit, was die API verlangt.**
  `POST /slots/{ams}/{tray}/configure` fordert `nozzle_temp_min/max`,
  `tray_type` und `tray_info_idx` — in der Oberfläche gibt die niemand ein.
  Bambuddys JavaScript leitet sie aus dem Profilnamen ab: Temperaturen aus
  einer festen Materialtabelle (PLA 190–230, PETG/PCTG 220–260, ABS/ASA
  240–280, TPU 200–240, PC 260–300, PA 250–290, sonst 190–230), bei lokalen
  Presets aus deren eigenen Werten, falls vorhanden. `tray_info_idx` ist bei
  integrierten Filamenten die `filament_id`, bei lokalen Presets **nicht**
  die Preset-ID, sondern die generische Kurz-ID des Materials
  (`PLA → GFL99`). `bambuddy_filament.cpp` bildet das nach — weicht es ab,
  bekommt derselbe Slot je nach Bedienweg andere Temperaturen.
  Nachzulesen im ausgelieferten Frontend: `{BASE_URL}/assets/index-*.js`,
  Suche nach `configureAmsSlot`. Bei Zweifeln über das Verhalten der
  Oberfläche ist das die verlässlichste Quelle — verlässlicher als die
  API-Beschreibung, die bei `tray_info_idx` in die Irre führt.
- **Manche Endpunkte sind für API-Schlüssel gesperrt**, unabhängig von
  deren Rechten. `PUT /printers/{id}/slot-presets/{ams}/{tray}` antwortet
  mit `403 {"detail":"API keys cannot be used for administrative
  operations"}` — lesen geht, schreiben nie. Das Display verzichtet deshalb
  darauf, die Zuordnung Slot → Profil zu vermerken. Bei einem 403 also
  zuerst den Klartext lesen: „ohne Cloud-Zugriff" ist eine Einstellung am
  Schlüssel, „administrative operations" dagegen eine Regel des Servers, an
  der sich nichts drehen lässt.
- **`state` ist ein freier String** vom Drucker (IDLE, RUNNING, PAUSE,
  FINISH, FAILED, PREPARE), kein Enum der API. Unbekannte Werte müssen
  durchgereicht statt verschluckt werden.

## Genutzte Endpunkte (Stand v1.2.5.3)

| Endpunkt | Datei |
|---|---|
| `GET /printers/{id}/status` | `bambuddy_api.cpp`, `bambuddy_queue.cpp` |
| `POST /printers/{id}/print/{pause,resume,stop}` | `bambuddy_api.cpp` |
| `POST /printers/{id}/chamber-light?on=` | `bambuddy_api.cpp` |
| `POST /printers/{id}/print-speed?mode=` | `bambuddy_api.cpp` |
| `POST /printers/{id}/{xy-jog,bed-jog,extruder-jog,home-axes}` | `bambuddy_api.cpp` |
| `POST /printers/{id}/clear-plate` | `bambuddy_queue.cpp`, `bambuddy_archive.cpp` |
| `GET /printers/{id}/cover` · `camera/snapshot` | `bambuddy_cover.cpp`, `bambuddy_camera.cpp` |
| `GET /queue/` · `POST /queue/` · `POST /queue/{id}/start` · `DELETE /queue/{id}` | `bambuddy_queue.cpp`, `bambuddy_archive.cpp` |
| `GET /archives/` · `DELETE /archives/{id}` · `GET /archives/{id}/thumbnail` | `bambuddy_archive.cpp`, `bambuddy_cover.cpp` |
| `GET /smart-plugs/` · `/{id}/status` · `POST /{id}/control` | `bambuddy_smart_plugs.cpp` |
| `GET /cloud/builtin-filaments` · `GET /local-presets/` | `bambuddy_filament.cpp` |
| `GET /printers/{id}/slot-presets` | `bambuddy_filament.cpp` |
| `POST /printers/{id}/slots/{ams}/{tray}/configure` · `POST /printers/{id}/ams/{ams}/tray/{tray}/reset` | `bambuddy_filament.cpp` |
| `GET /updates/check` (Ersatz: `GET /updates/version`) | `bambuddy_version.cpp` |

MQTT liest denselben Status über das Topic aus den Einstellungen
(`bambuddy/printers/{serial}/status`) — dieselben Feldnamen wie die
HTTP-Antwort, nur `printer_name` statt `name`. Ausgewertet wird beides von
`bambuddy_status_from_json()` in `bambuddy_status_parse.h`.

---

## Arbeitsweise

- **Nicht bauen oder flashen.** `pio run` macht Rico selbst nach jeder
  Änderung; Änderungen also schreiben und kurz zusammenfassen.
- Nach Änderungen an `include/lv_conf.h` muss `.pio/build` gelöscht werden,
  sonst greifen sie nicht.
