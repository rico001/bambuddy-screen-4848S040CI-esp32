# Firmware Backup — ESP32-4848S040CI

Backup der Original-Firmware vom Sunton ESP32-4848S040CI Board (Werkszustand / Demo-Firmware).

## Wie wurde das Backup erstellt?

Mit `esptool.py` wurde der gesamte 16MB Flash-Speicher ausgelesen:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 read_flash 0x0 0x1000000 firmware_full_16MB.bin
```

- **Port**: `/dev/cu.usbserial-10` (kann bei dir anders sein, prüfe mit `ls /dev/cu.usb*`)
- **Baud**: 115200 (höhere Raten waren bei diesem Board instabil)
- **Adresse**: `0x0` — Start des Flash
- **Größe**: `0x1000000` = 16MB (gesamter Flash-Speicher)
- **Dauer**: ca. 20 Minuten bei 115200 Baud

## Wie kann man das Backup wiederherstellen?

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 \
    write_flash 0x0 firmware_full_16MB.bin
```

Falls der Chip vorher gelöscht werden soll (empfohlen bei Problemen):

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbserial-10 erase_flash

~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
    --chip esp32s3 --port /dev/cu.usbserial-10 --baud 115200 \
    write_flash 0x0 firmware_full_16MB.bin
```

## Hinweise

- Falls `esptool.py` nicht im PATH ist, liegt es bei PlatformIO unter:
  `~/.platformio/packages/tool-esptoolpy/esptool.py`
  und muss mit der PlatformIO-Python-Umgebung aufgerufen werden:
  `~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py ...`
- Das Board muss per USB verbunden sein und ggf. in den Download-Modus versetzt werden (BOOT-Taste gedrückt halten beim Einschalten)
- Die Datei `firmware_full_16MB.bin` enthält alles: Bootloader, Partitionstabelle, Applikation und ggf. SPIFFS/LittleFS-Daten
