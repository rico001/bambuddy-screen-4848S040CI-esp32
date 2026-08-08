# Demo Firmware Metadata — ESP32-4848S040CI Werksfirmware

Analyse der `firmware_full_16MB.bin` basierend auf String-Extraktion aus der Binary.

## Checksums

| Algorithmus | Hash |
|-------------|------|
| MD5         | `51ae73f610583f8583063289c9f20494` |
| SHA-256     | `2199fcca0834d5916d512745f8ef632dd3c7ef0f5105d6bd5e25ab2a7cfe3ccb` |
| Größe       | 16.777.216 Bytes (16 MB, vollständiger Flash-Dump) |

## Build-Umgebung

| Eigenschaft | Wert |
|-------------|------|
| Framework   | Arduino (via PlatformIO) |
| Platform    | `framework-arduinoespressif32@src-87caf4f9b2b7783aabe96b384b39944d` |
| ESP-IDF     | Commit `6b1f40b9bf` (eingebettet im Arduino-Framework) |
| Build-System | PlatformIO (Pfade: `/root/.platformio/...` — Linux/Docker) |
| Chip        | ESP32-S3 (Xtensa) |

## GUI / Display

| Eigenschaft | Wert |
|-------------|------|
| GUI-Framework | **LVGL 8.3.7** |
| Display-Library | **ESP32_Display_Panel** (nicht esp32-smartdisplay) |
| Touch-Treiber | GT911 (kapazitiv, I2C) |
| Display-Interface | RGB Panel (`esp_lcd_panel_rgb`) |
| UI-System | JSON-basiertes Scripting (Page-Visibility, Events, Timer) |

## Funktionen der Demo-Firmware

### Wetter-App (Hauptfunktion)
- Stadtsuche über **Open-Meteo Geocoding API** (`geocoding-api.open-meteo.com/v1/search`)
- Wetterdaten über **Open-Meteo Forecast API** (`api.open-meteo.com/v1/forecast`)
  - Temperatur, Luftfeuchtigkeit, Wettercode, Windgeschwindigkeit
- Zeitabfrage über **Taobao API** (`acs.m.taobao.com/gw/mtop.common.getTimestamp`)

### Konnektivität
- **WiFi** (STA-Modus mit Scan-Funktion)
- **HTTPClient** (HTTP-Requests zu Wetter-APIs)
- **WiFiClientSecure** (HTTPS/TLS)
- **OTA-Updates** (HTTPS OTA)

### Weitere Features
- **QR-Code-Generator** (`qrcodegen`)
- **NVS** (Non-Volatile Storage für Einstellungen)
- **Temperatur-Sensor** (interner ESP32-S3 Sensor)
- Mehrere Screens/Pages (p4, p55–p62) mit Sichtbarkeits-Steuerung per Wettercode

## Hardware-Konfiguration

### Display
| Eigenschaft | Wert |
|-------------|------|
| Display-Controller | **ST7701** (SPI-Init + RGB-Interface) |
| Display-Interface | RGB Panel (`esp_lcd_panel_rgb`) |
| Auflösung | 480×480 Pixel |
| Farbtiefe | RGB565 (16-bit) |

### Touch
| Eigenschaft | Wert |
|-------------|------|
| Touch-Controller | **GT911** |
| Interface | I2C (`esp_lcd_touch_new_i2c_gt911`) |

### Backlight
| Eigenschaft | Wert |
|-------------|------|
| Steuerung | LEDC (PWM) via `ESP_PanelBacklight` |

### GPIO-Belegung (aus JSON-Scripting)

Die Demo-Firmware steuert über das JSON-UI-System folgende GPIOs direkt:

| GPIO | Funktion | Bemerkung |
|------|----------|-----------|
| GPIO 40 | Relais / Ausgang 1 | Toggle On/Off in der UI |
| GPIO 1  | Relais / Ausgang 2 | Toggle On/Off in der UI |
| GPIO 2  | Relais / Ausgang 3 | Toggle On/Off in der UI |

Diese drei GPIOs werden als digitale Ausgänge (High/Low) per Touch-Button geschaltet — vermutlich die drei Relais-Ausgänge des Boards.

### Speicher
| Eigenschaft | Wert |
|-------------|------|
| Flash | 16 MB |
| PSRAM | Embedded PSRAM (Octal SPI / OPI) |
| NVS | Vorhanden (WiFi-Credentials, Kalibrierung) |
| Partitionen | Mindestens: App, NVS, OTA |

### Hinweis zu Pin-Belegungen

Die vollständigen Pin-Zuweisungen für Display (RGB-Datenleitungen, HSYNC, VSYNC, PCLK, DE), Touch (SDA, SCL, INT, RST) und Backlight sind **nicht als Klartext-Strings** in der Binary enthalten. Sie sind als kompilierte Konstanten in der ESP32_Display_Panel Library eingebettet. Die Board-Definition in unserer `boards/`-JSON oder der esp32-smartdisplay Library ist die bessere Quelle für die vollständige Pin-Belegung.

## Unterschiede zu unserem Projekt

| Aspekt | Werksfirmware | Unser Projekt |
|--------|--------------|---------------|
| LVGL-Version | 8.3.7 | 9.2.2 |
| Display-Library | ESP32_Display_Panel | esp32-smartdisplay |
| UI-Aufbau | JSON-Scripting | C++ mit LVGL-API |
| Features | Wetter-App mit WiFi | Demo/Playground |
