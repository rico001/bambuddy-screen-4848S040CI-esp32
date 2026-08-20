#include "ui_theme.h"

#include <lvgl.h>

// Alle Toene entstehen aus einer einzigen Farbe.
//
// Frueher standen hier zwei feste Paletten. Seit die Akzentfarbe einstellbar
// ist, waeren das zwei Tabellen, die bei jeder Farbe von Hand nachgezogen
// werden muessten — also werden sie gerechnet: Der Farbwinkel bleibt, nur
// Saettigung und Helligkeit werden gestaffelt.
//
// Warum ueber HSV und nicht ueber "RGB mal 0,2": Ein abgedunkeltes RGB
// verliert seinen Farbcharakter und kippt ins Graue. Ueber den Farbwinkel
// bleibt Blau blau, auch bei acht Prozent Helligkeit.

static uint32_t primary = COL_PRIMARY_DEFAULT;
static bool theme_dark = true;

uint32_t COL_BG, COL_SURFACE, COL_RAISED, COL_LINE;
uint32_t COL_TEXT, COL_MUTED;
uint32_t COL_ACCENT;

// Die Bedeutungsfarben haengen an ihrer Aussage, nicht an der Akzentfarbe.
// Sie wechseln nur zwischen hellem und dunklem Schema, weil ein Gruen, das
// auf Schwarz leuchtet, auf Weiss blass wirkt.
uint32_t COL_OK, COL_WARN, COL_ERR, COL_NEUTRAL, COL_PLUG, COL_JOG;

uint32_t COL_RAIN_HEAD;
uint32_t COL_RAIN[COL_RAIN_STEPS];
uint32_t COL_SAVER_PANEL, COL_SAVER_PILL, COL_SAVER_TRACK, COL_SAVER_MUTED;

static uint32_t hsv(int h, int s, int v)
{
    const lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(h % 360), (uint8_t)s, (uint8_t)v);
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

static void recompute()
{
    const lv_color_hsv_t base = lv_color_rgb_to_hsv((uint8_t)(primary >> 16),
                                                    (uint8_t)(primary >> 8),
                                                    (uint8_t)primary);
    const int h = base.h;

    // Eine sehr blasse Farbe (etwa Weiss) gaebe sonst graue Flaechen ohne
    // jeden Charakter. Fuer die Herleitung wird deshalb eine Mindestsaettigung
    // angenommen — die Akzentfarbe selbst bleibt unangetastet.
    const int s = base.s < 25 ? 25 : base.s;

    COL_ACCENT = primary;

    if (theme_dark) {
        // Von unten nach oben heller, die Saettigung dabei leicht fallend:
        // Ganz unten darf der Ton kraeftiger sein, weil grosse Flaechen sonst
        // farblos wirken; weiter oben wuerde dieselbe Saettigung schmutzig
        // aussehen.
        COL_BG      = hsv(h, s * 45 / 100, 9);
        COL_SURFACE = hsv(h, s * 38 / 100, 14);
        COL_RAISED  = hsv(h, s * 32 / 100, 20);
        COL_LINE    = hsv(h, s * 28 / 100, 27);
        COL_TEXT    = hsv(h, s * 8 / 100, 95);
        COL_MUTED   = hsv(h, s * 15 / 100, 66);
        COL_NEUTRAL = hsv(h, s * 25 / 100, 32);

        COL_OK   = 0x22C55E;
        COL_WARN = 0xF59E0B;
        COL_ERR  = 0xEF4444;
        COL_PLUG = 0xF97316;
        COL_JOG  = 0x6366F1;
    } else {
        COL_BG      = hsv(h, s * 12 / 100, 97);
        COL_SURFACE = hsv(h, s * 5 / 100, 100);
        COL_RAISED  = hsv(h, s * 14 / 100, 93);
        COL_LINE    = hsv(h, s * 20 / 100, 85);
        COL_TEXT    = hsv(h, s * 40 / 100, 12);
        COL_MUTED   = hsv(h, s * 25 / 100, 45);
        COL_NEUTRAL = hsv(h, s * 18 / 100, 82);

        COL_OK   = 0x15803D;
        COL_WARN = 0xB45309;
        COL_ERR  = 0xDC2626;
        COL_PLUG = 0xC2410C;
        COL_JOG  = 0x4338CA;
    }

    // Der Regen: vom fast weissen Kopf in vier Stufen bis zur Akzentfarbe.
    //
    // Die letzte Stufe ist bewusst dunkler als der Akzent selbst — das Ende
    // der Spur soll verklingen und nicht so kraeftig stehen wie der Kopf des
    // naechsten Tropfens daneben.
    COL_RAIN_HEAD = hsv(h, s * 15 / 100, 100);

    // Die Tafel des Schoners: unabhaengig vom Farbschema dunkel gehalten.
    COL_SAVER_PANEL = hsv(h, s * 55 / 100, 13);
    COL_SAVER_PILL  = hsv(h, s * 60 / 100, 10);
    COL_SAVER_TRACK = hsv(h, s * 50 / 100, 24);
    COL_SAVER_MUTED = hsv(h, s * 45 / 100, 72);

    const int sat_from = s * 45 / 100, sat_to = s;
    const int val_from = 92, val_to = (base.v * 70) / 100;
    for (int i = 0; i < COL_RAIN_STEPS; i++) {
        const int t = i * 100 / (COL_RAIN_STEPS - 1); // 0 .. 100
        COL_RAIN[i] = hsv(h, sat_from + (sat_to - sat_from) * t / 100,
                          val_from + (val_to - val_from) * t / 100);
    }
}

void ui_theme_set_dark(bool dark)
{
    theme_dark = dark;
    recompute();
}

bool ui_theme_is_dark()
{
    return theme_dark;
}

void ui_theme_set_primary(uint32_t rgb)
{
    primary = rgb ? rgb : COL_PRIMARY_DEFAULT;
    recompute();
}

uint32_t ui_theme_primary()
{
    return primary;
}
