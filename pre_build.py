import os
from datetime import datetime

Import("env")

# Remove ARM-specific assembly files (Helium, NEON) incompatible with Xtensa (ESP32)
def remove_arm_asm(*args, **kwargs):
    libdeps_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))
    arm_dirs = ["helium", "neon"]
    for root, dirs, files in os.walk(libdeps_dir):
        for f in files:
            if f.endswith(".S") and any(d in root for d in arm_dirs):
                path = os.path.join(root, f)
                if os.path.exists(path):
                    os.remove(path)
                    print(f"Removed incompatible ARM assembly file: {path}")

remove_arm_asm()


# Version und Bauzeitpunkt in die Firmware schreiben.
#
# Ein __DATE__ irgendwo im Quelltext taugt dafuer nicht: Die Datei wird nur
# neu uebersetzt, wenn sie sich aendert, und zeigt sonst den Zeitpunkt des
# letzten Mals. Aus demselben Grund ist auch der App-Deskriptor der IDF
# nutzlos — sein Datum stammt aus Espressifs Build der vorkompilierten
# Bibliothek, nicht aus diesem hier.
#
# Deshalb wird die eine kleine Datei bei jedem Lauf neu geschrieben. Ihr
# Inhalt aendert sich durch den Zeitstempel immer, also uebersetzt SCons
# genau sie neu — und nur sie.
#
# Als Version gilt, was in version.txt steht. Dieselbe Zahl benutzt
# release.sh fuer den Dateinamen in firmware-build/, damit die Datei auf der
# Platte und die Anzeige auf dem Geraet dieselbe Fassung meinen.
def generate_build_stamp():
    project_dir = env.subst("$PROJECT_DIR")
    version = "unbekannt"

    version_file = os.path.join(project_dir, "version.txt")
    if os.path.exists(version_file):
        with open(version_file, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#"):
                    version = line
                    break

    stamp = datetime.now().strftime("%d.%m.%Y, %H:%M")
    target = os.path.join(project_dir, "src", "build_stamp.cpp")

    with open(target, "w", encoding="utf-8") as fh:
        fh.write(
            "// Erzeugt von pre_build.py bei jedem Build. Nicht von Hand aendern —\n"
            "// die Datei wird beim naechsten 'pio run' ueberschrieben.\n"
            '#include "build_stamp.h"\n\n'
            f'const char *build_stamp_version() {{ return "{version}"; }}\n'
            f'const char *build_stamp_datetime() {{ return "{stamp}"; }}\n'
        )

    print(f"Build-Stempel: v{version}, {stamp}")


generate_build_stamp()
