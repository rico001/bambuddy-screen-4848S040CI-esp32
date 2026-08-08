#pragma once

#include <stdint.h>

// Lebenszeichen des UI-Threads.
//
// Bleibt loopTask haengen, hilft der Task-Watchdog nur begrenzt: Er meldet
// zwar, dass der Task steht, sein Backtrace zeigt aber die Unterbrechung —
// nicht die Stelle, an der es klemmt. Deshalb hinterlaesst der UI-Thread
// hier laufend einen Merker, und der Netzwerk-Task auf dem anderen Kern
// schreibt ihn heraus, sobald das Lebenszeichen ausbleibt.

extern volatile uint32_t ui_watch_alive_ms;
extern const char *volatile ui_watch_step;

// Merker setzen. Kostet einen Zeigerzugriff — bewusst so billig, dass er
// auch in haeufig durchlaufenen Stellen stehen bleiben kann.
static inline void ui_watch(const char *step)
{
    ui_watch_step = step;
}
