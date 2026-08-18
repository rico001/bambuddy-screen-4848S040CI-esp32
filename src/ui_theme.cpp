#include "ui_theme.h"

// Zwei Saetze derselben Bedeutungen.
//
// Das dunkle Schema ist der Alltagsfall: Das Geraet haengt an der Wand, oft
// in einem Raum ohne Deckenlicht. Das helle ist fuer die Werkstatt am Fenster
// — dort verschluckt Sonnenlicht die dunklen Abstufungen.
//
// Die Bedeutungsfarben sind im hellen Schema nicht dieselben Werte: Ein Gruen,
// das auf Schwarz leuchtet, wirkt auf Weiss blass, und ein Gelb, das auf
// Schwarz warnt, ist auf Weiss kaum zu sehen. Gleich bleibt, was sie
// bedeuten, nicht ihr Zahlenwert.

struct palette_t {
    uint32_t bg, surface, raised, line;
    uint32_t text, muted;
    uint32_t ok, warn, err, accent, neutral, plug, jog;
};

static constexpr palette_t DARK = {
    0x0E1116, 0x171B22, 0x1F2530, 0x272E3A,
    0xE8ECF2, 0x8A94A6,
    0x22C55E, 0xF59E0B, 0xEF4444, 0x3B82F6, 0x38414F, 0xF97316, 0x6366F1,
};

static constexpr palette_t LIGHT = {
    0xF4F6FA, 0xFFFFFF, 0xEDF0F5, 0xD9DEE7,
    0x161A20, 0x5C6675,
    0x15803D, 0xB45309, 0xDC2626, 0x1D4ED8, 0xCBD2DC, 0xC2410C, 0x4338CA,
};

// Startwerte: dunkel. Kaeme das Geraet vor dem ersten ui_theme_set_dark()
// dazu, etwas zu zeichnen, waere das der Zustand, den der Nutzer in neun von
// zehn Faellen ohnehin eingestellt hat.
uint32_t COL_BG = DARK.bg;
uint32_t COL_SURFACE = DARK.surface;
uint32_t COL_RAISED = DARK.raised;
uint32_t COL_LINE = DARK.line;
uint32_t COL_TEXT = DARK.text;
uint32_t COL_MUTED = DARK.muted;
uint32_t COL_OK = DARK.ok;
uint32_t COL_WARN = DARK.warn;
uint32_t COL_ERR = DARK.err;
uint32_t COL_ACCENT = DARK.accent;
uint32_t COL_NEUTRAL = DARK.neutral;
uint32_t COL_PLUG = DARK.plug;
uint32_t COL_JOG = DARK.jog;

static bool theme_dark = true;

void ui_theme_set_dark(bool dark)
{
    theme_dark = dark;
    const palette_t &p = dark ? DARK : LIGHT;

    COL_BG = p.bg;
    COL_SURFACE = p.surface;
    COL_RAISED = p.raised;
    COL_LINE = p.line;
    COL_TEXT = p.text;
    COL_MUTED = p.muted;
    COL_OK = p.ok;
    COL_WARN = p.warn;
    COL_ERR = p.err;
    COL_ACCENT = p.accent;
    COL_NEUTRAL = p.neutral;
    COL_PLUG = p.plug;
    COL_JOG = p.jog;
}

bool ui_theme_is_dark()
{
    return theme_dark;
}
