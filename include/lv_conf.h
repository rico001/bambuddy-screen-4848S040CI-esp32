/**
 * LVGL configuration file for ESP32-4848S040CIY1
 * Based on lv_conf_template.h from LVGL 9.1
 */

/* clang-format off */
#if 1 /*Set it to "1" to enable content*/

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/

/*Color depth: 1 (1 byte per pixel), 8 (RGB332), 16 (RGB565), 32 (ARGB8888)*/
#define LV_COLOR_DEPTH 16

/*=========================
   MEMORY SETTINGS
 *=========================*/

/* Kein fester eigener Pool, sondern der normale Heap des Systems.
 *
 * Der eingebaute Allokator belegt einen festen Block im internen RAM — und
 * der ist auf diesem Board bereits durch den 128-KB-DMA-Puffer des Displays
 * und den WLAN-Stack knapp. Beide Groessen gegeneinander abzuwaegen fuehrt
 * in eine Sackgasse: Ein grosser Pool laesst Sockets scheitern, ein kleiner
 * laesst LVGL beim Anfordern eines Zeichenpuffers NULL bekommen — und LVGL
 * prueft das nicht, sondern schreibt an Adresse 0 (Absturz im lv_memset).
 *
 * Mit LV_STDLIB_CLIB geht beides in denselben Heap: Kleine Anforderungen
 * (Objekte, Styles) bleiben intern, alles ueber 4 KB legt ESP-IDF
 * automatisch ins PSRAM — also genau die grossen Zeichenpuffer. Damit
 * entfaellt die Abwaegung, und der interne RAM steht dem Netzwerk zur
 * Verfuegung. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB

/* Ohne eigenen Pool wirkungslos — bleibt fuer den Fall stehen, dass wieder
 * auf LV_STDLIB_BUILTIN umgestellt wird. */
#define LV_MEM_SIZE (56U * 1024U)          /*[bytes]*/

/*Size of the memory expand for `lv_malloc()` in bytes*/
#define LV_MEM_POOL_EXPAND_SIZE 0

/*Set an address for the memory pool instead of allocating it as a normal array. Can be in external SRAM too.*/
#define LV_MEM_ADR 0     /*0: unused*/

/*Instead of an address give a memory allocator that will be called to get a memory pool for LVGL. E.g. my_malloc*/
#if LV_MEM_ADR == 0
    #undef LV_MEM_POOL_INCLUDE
    #undef LV_MEM_POOL_ALLOC
#endif

#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/*====================
   HAL SETTINGS
 *====================*/

/*Default display refresh, input device read and animation step period.*/
#define LV_DEF_REFR_PERIOD  33      /*[ms]*/
#define LV_DPI_DEF 130     /*[px/inch]*/

/*=================
 * OPERATING SYSTEM
 *=================*/
#define LV_USE_OS   LV_OS_NONE

/*========================
 * RENDERING CONFIGURATION
 *========================*/
#define LV_DRAW_BUF_STRIDE_ALIGN        1
#define LV_DRAW_BUF_ALIGN               4
#define LV_USE_DRAW_SW  1

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

/*-------------
 * Logging
 *-----------*/
#define LV_USE_LOG 0
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 0
    #define LV_LOG_USE_TIMESTAMP 1
    #define LV_LOG_USE_FILE_LINE 1
    #define LV_USE_LOG_TRACE_MEM        1
    #define LV_USE_LOG_TRACE_TIMER      1
    #define LV_USE_LOG_TRACE_INDEV      1
    #define LV_USE_LOG_TRACE_DISP_REFR  1
    #define LV_USE_LOG_TRACE_EVENT      1
    #define LV_USE_LOG_TRACE_OBJ_CREATE 1
    #define LV_USE_LOG_TRACE_LAYOUT     1
    #define LV_USE_LOG_TRACE_ANIM       1
#endif  /*LV_USE_LOG*/

/*-------------
 * Asserts
 *-----------*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE   <stdint.h>
#define LV_ASSERT_HANDLER while(1);

/*-------------
 * Debug
 *-----------*/
#define LV_USE_SYSMON 0

/*-------------
 * Others
 *-----------*/
#define LV_USE_USER_DATA 1
#define LV_ENABLE_GLOBAL_CUSTOM 0
#define LV_USE_FLOAT            0

/*==================
 * WIDGETS
 *================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BUTTON     1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CALENDAR   1
#define LV_USE_CANVAS     1
#define LV_USE_CHART      1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMAGE      1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_KEYBOARD   1
#define LV_USE_LABEL      1
#define LV_USE_LED        1
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_MENU       1
#define LV_USE_MSGBOX     1
#define LV_USE_OBJID      0
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      1
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       1
#define LV_USE_SPINBOX    1
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1
#define LV_USE_TABLE      1
#define LV_USE_TABVIEW    1
#define LV_USE_TEXTAREA   1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        1

/*==================
 * THEMES
 *================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_USE_THEME_SIMPLE  1

/*==================
 * FONT USAGE
 *================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1 // Bildschirmschoner: grosse Uhrzeit

#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

// Eigene Schnitte statt der eingebauten Montserrat-Fonts: Die mitgelieferten
// kennen nur ASCII und zeigen fuer Umlaute ein leeres Rechteck. Siehe
// src/ui_font.h. Groesse 48 (Uhr im Schoner) bleibt eingebaut, dort stehen
// nur Ziffern.
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(bb_font_12) LV_FONT_DECLARE(bb_font_14) LV_FONT_DECLARE(bb_font_16) LV_FONT_DECLARE(bb_font_24)
#define LV_FONT_DEFAULT &bb_font_14

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX     0

/*==================
 * TEXT SETTINGS
 *================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD "#"

/*==================
 * LIBS
 *================*/
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF 0
#define LV_BIN_DECODER_RAM_LOAD 0
#define LV_USE_RLE 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_LZ4_INTERNAL  0
#define LV_USE_LZ4_EXTERNAL  0
#define LV_USE_FFMPEG 0

#endif /*LV_CONF_H*/

#endif /*End of "Content enable"*/
