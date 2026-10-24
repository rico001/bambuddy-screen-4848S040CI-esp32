#!/usr/bin/env bash
# Baut die Firmware und schickt sie per Web-Update aufs Display.
# Adresse steht am Geraet unter Einstellungen -> FIRMWARE -> Web-Update.
#
#   sh ota-updat-example.sh 192.168.1.23
./release.sh --build --upload "${1:?Adresse des Displays angeben, z.B.: sh ota-updat-example.sh 192.168.1.23}" --force
