/*****************************************************************************
** File: screen.h
** Description: Screen device interface for SimpleGUI on STM32F1 + SSD1306 I2C.
**              Implements the SGUI_SCR_DEV callbacks required by SimpleGUI.
**
** Memory model:
**   - 1KB frame buffer (128 x 64 / 8) held in RAM as ui8DisplayCache
**   - A dirty-region record (page + column range) so only touched pages are
**     pushed to the panel, which matters a lot over slow 100kHz I2C.
*****************************************************************************/
#ifndef __SCREEN_H__
#define __SCREEN_H__

#include "SGUI_Typedef.h"
#include "ssd1306.h"

/* Screen color, matching SGUI_COLOR_FRGCLR / SGUI_COLOR_BKGCLR */
#define SCREEN_COLOR_BKG           (0)
#define SCREEN_COLOR_FRG           (1)

/* Initialize the panel and the frame buffer. */
void SCREEN_Initialize(void);

/* 只清显存（不动面板），并把脏区记录一并复位。
 * 换屏时用它 —— 脏区记录不复位的话，上一次的脏区范围会把旧内容又推回屏幕。 */
void SCREEN_ClearCache(void);

/* SGUI_SCR_DEV callbacks */
void SCREEN_SetPixel(int iPosX, int iPosY, unsigned int uiColor);
int  SCREEN_GetPixel(int iPosX, int iPosY);
void SCREEN_ClearDisplay(void);
void SCREEN_FillRectangle(int iPosX, int iPosY, int iWidth, int iHeight, unsigned int uiColor);
void SCREEN_RefreshScreen(void);

/* Device interface object used by SimpleGUI. */
extern SGUI_SCR_DEV g_stDeviceInterface;

#endif /* __SCREEN_H__ */
