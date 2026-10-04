/*****************************************************************************
** File: RTC.h
** Description: Minimal RTC placeholder for the SimpleGUI hardware port.
**
** Why this file exists:
**   GUI/src/SGUI_Interface.c includes "RTC.h" when the virtual SDK is off,
**   as a hook point for real-time calendar functions used by date/time
**   widgets. In the current port those widgets are not used, and the file is
**   never actually called, so a placeholder is enough.
**
** If you later enable SGUI_VariableBox date/time features, implement the
** calendar read here (see Transplant/MiniDevCore/BSP/inc/rtc.h in the SimpleGUI
** repo for the reference implementation using STM32F1 BKP/RTC peripherals).
*****************************************************************************/
#ifndef _INCLUDE_RTC_H_
#define _INCLUDE_RTC_H_

#include <stdint.h>

/* Calendar structure, mirrors "struct tm" of the C library. */
typedef struct tm   RTC_CALENDAR_STRUCT;

/* Uncomment and implement when date/time widgets are needed. */
/*
uint32_t                RTC_GetTimeStamp(void);
RTC_CALENDAR_STRUCT*    RTC_ConvertToCalendar(uint32_t uiTimeStamp);
*/

#endif /* _INCLUDE_RTC_H_ */
