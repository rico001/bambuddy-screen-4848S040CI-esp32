#pragma once

// Gemeinsame Bildschirmmasse. Die Statusleiste liegt fest oben, darunter
// bleibt der Platz fuer die Screens — die duerfen nicht mehr von 480 Pixel
// Hoehe ausgehen, sonst rutscht ihr unteres Ende aus dem Bild.

static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 480;
static constexpr int STATUS_BAR_H = 26;
static constexpr int CONTENT_H = SCREEN_H - STATUS_BAR_H; // 454
