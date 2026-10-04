#ifndef __SGUI_FONT_GB2312_H__
#define __SGUI_FONT_GB2312_H__
/*****************************************************************************
** File: sgui_font_gb2312.h
** Description: Trimmed GB2312 (FZXS-12) font for STM32F1 + SSD1306 demo.
**
** Why trimmed: the full font in DemoProc/src/Resource.c is 183876 bytes
** (7661 glyphs, ~180 KB). A STM32F103C8T6 only has 64 KB of Flash, so the
** full set cannot fit. This version keeps ASCII 0x20..0x7E plus only the
** Chinese characters the demo prints, which is about 2.3 KB.
**
** Glyph metrics match the original resource:
**   iHalfWidth = 6, iFullWidth = 12, iHeight = 12
**   half-width glyph = 12 bytes, full-width glyph = 24 bytes
**
** Text must be GB2312 encoded in the source, e.g. "\D6\D0" for one character,
** where \ denotes a backslash.
*****************************************************************************/
#include "SGUI_Typedef.h"

#ifdef __cplusplus
extern "C"{
#endif

/* Font resource object. Pass &SGUI_FONT_GB2312 to the SGUI_Text_* functions. */
extern const SGUI_FONT_RES SGUI_FONT_GB2312;

#ifdef __cplusplus
}
#endif

#endif /* __SGUI_FONT_GB2312_H__ */
