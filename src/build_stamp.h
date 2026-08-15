#pragma once

// Welche Fassung laeuft hier, und wann wurde sie gebaut?
//
// Beides erzeugt pre_build.py bei jedem Build neu in src/build_stamp.cpp:
// die Version aus version.txt, den Zeitstempel aus der Uhr des Rechners.
// Die erzeugte Datei steht nicht im Git — sie gehoert zum Build, nicht zum
// Quelltext.
//
// Gebraucht wird das von der Update-Seite: Nach einem Web-Update ist die
// erste Frage immer, ob wirklich das neue Abbild laeuft.
const char *build_stamp_version();  // "1.0.0"
const char *build_stamp_datetime(); // "16.08.2026, 01:12"
