/*****************************************************************************
** File: screen.c
** Description: Screen device interface - implements SGUI_SCR_DEV callbacks.
**              Buffer layout is page-linear: ui8DisplayCache[page * 128 + col],
**              which matches the SSD1306 page addressing mode, so a full page
**              can be pushed in one I2C transaction.
*****************************************************************************/
#include "screen.h"
#include <string.h>

SGUI_SCR_DEV g_stDeviceInterface;

/*-------------------------------- Private -------------------------------*/
/* Frame buffer: 128 x 64 / 8 = 1024 bytes. */
static uint8_t s_aucDisplayCache[SSD1306_WIDTH * SSD1306_PAGES];

/* Dirty region. Initialized to an empty range (start beyond end). */
static struct
{
    uint8_t     uiStartPage;
    uint8_t     uiEndPage;
    uint8_t     uiStartColumn;
    uint8_t     uiEndColumn;
}s_stUpdateArea;

#define CACHE_INDEX(page, col)    ((uint16_t)((page) * SSD1306_WIDTH + (col)))

static void SCREEN_RecordDirty(uint8_t uiPage, uint8_t uiColumn)
{
    if(uiPage < s_stUpdateArea.uiStartPage)
    {
        s_stUpdateArea.uiStartPage = uiPage;
    }
    if(uiPage > s_stUpdateArea.uiEndPage)
    {
        s_stUpdateArea.uiEndPage = uiPage;
    }
    if(uiColumn < s_stUpdateArea.uiStartColumn)
    {
        s_stUpdateArea.uiStartColumn = uiColumn;
    }
    if(uiColumn > s_stUpdateArea.uiEndColumn)
    {
        s_stUpdateArea.uiEndColumn = uiColumn;
    }
}

static void SCREEN_ClearDirtyRecord(void)
{
    s_stUpdateArea.uiStartPage      = SSD1306_PAGES;
    s_stUpdateArea.uiEndPage        = 0;
    s_stUpdateArea.uiStartColumn    = SSD1306_WIDTH;
    s_stUpdateArea.uiEndColumn      = 0;
}

/*-------------------------------- Public --------------------------------*/
/*****************************************************************************
** Function Name: SCREEN_ClearCache
** Purpose:       Clear the frame buffer only (no panel access).
** Return:        None
*****************************************************************************/
void SCREEN_ClearCache(void)
{
    memset(s_aucDisplayCache, 0x00, sizeof(s_aucDisplayCache));
    SCREEN_ClearDirtyRecord();
}

/*****************************************************************************
** Function Name: SCREEN_Initialize
** Purpose:       Initialize the panel and the frame buffer.
** Return:        None
*****************************************************************************/
void SCREEN_Initialize(void)
{
    SCREEN_ClearCache();
    SSD1306_Init();
}

/*****************************************************************************
** Function Name: SCREEN_SetPixel
** Purpose:       Set one pixel in the frame buffer and mark the page dirty.
**                Required by SimpleGUI (fnSetPixel).
** Params:       iPosX - column, iPosY - row, iColor - SCREEN_COLOR_FRG/BKG
** Return:        None
*****************************************************************************/
void SCREEN_SetPixel(int iPosX, int iPosY, unsigned int uiColor)
{
    uint16_t usIndex;

    if((iPosX < 0) || (iPosX >= SSD1306_WIDTH) || (iPosY < 0) || (iPosY >= SSD1306_HEIGHT))
    {
        return;
    }
    usIndex = CACHE_INDEX(iPosY / 8, iPosX);
    if(SCREEN_COLOR_FRG == (int)uiColor)
    {
        s_aucDisplayCache[usIndex] |= (uint8_t)(1 << (iPosY % 8));
    }
    else
    {
        s_aucDisplayCache[usIndex] &= (uint8_t)~(1 << (iPosY % 8));
    }
    SCREEN_RecordDirty((uint8_t)(iPosY / 8), (uint8_t)iPosX);
}

/*****************************************************************************
** Function Name: SCREEN_GetPixel
** Purpose:       Read one pixel back from the frame buffer.
**                Required by SimpleGUI when SGUI_GET_POINT_FUNC_EN is enabled.
** Note:          SSD1306 has no hardware read-back, so this reads the RAM
**                cache instead. That is valid because the cache is the
**                authoritative copy of what is displayed.
** Return:        SCREEN_COLOR_FRG or SCREEN_COLOR_BKG
*****************************************************************************/
int SCREEN_GetPixel(int iPosX, int iPosY)
{
    if((iPosX < 0) || (iPosX >= SSD1306_WIDTH) || (iPosY < 0) || (iPosY >= SSD1306_HEIGHT))
    {
        return SCREEN_COLOR_BKG;
    }
    return ((s_aucDisplayCache[CACHE_INDEX(iPosY / 8, iPosX)] >> (iPosY % 8)) & 0x01)
            ? SCREEN_COLOR_FRG : SCREEN_COLOR_BKG;
}

/*****************************************************************************
** Function Name: SCREEN_ClearDisplay
** Purpose:       Clear frame buffer and panel. Required by SimpleGUI (fnClear).
** Return:        None
*****************************************************************************/
void SCREEN_ClearDisplay(void)
{
    SCREEN_ClearCache();
    SSD1306_Clear();
}

/*****************************************************************************
** Function Name: SCREEN_FillRectangle
** Purpose:       Fill a rectangle directly in the frame buffer.
**                Optional (fnFillRect); SimpleGUI falls back to SetPixel.
** Return:        None
*****************************************************************************/
void SCREEN_FillRectangle(int iPosX, int iPosY, int iWidth, int iHeight, unsigned int uiColor)
{
    int iX, iY;
    uint8_t ucMask;
    uint16_t usIndex;

    if((iWidth <= 0) || (iHeight <= 0))
    {
        return;
    }
    /* Fill byte-aligned spans wholesale, handle the partial head/tail bytes. */
    for(iY = iPosY; iY < (iPosY + iHeight); iY++)
    {
        for(iX = iPosX; iX < (iPosX + iWidth); iX++)
        {
            if((iX < 0) || (iX >= SSD1306_WIDTH) || (iY < 0) || (iY >= SSD1306_HEIGHT))
            {
                continue;
            }
            usIndex = CACHE_INDEX(iY / 8, iX);
            ucMask  = (uint8_t)(1 << (iY % 8));
            if(SCREEN_COLOR_FRG == (int)uiColor)
            {
                s_aucDisplayCache[usIndex] |= ucMask;
            }
            else
            {
                s_aucDisplayCache[usIndex] &= (uint8_t)~ucMask;
            }
            SCREEN_RecordDirty((uint8_t)(iY / 8), (uint8_t)iX);
        }
    }
}

/*****************************************************************************
** Function Name: SCREEN_RefreshScreen
** Purpose:       Push the dirty region to the panel. Required by SimpleGUI
**                (fnSyncBuffer). This is the core of the local-refresh design.
** Return:        None
*****************************************************************************/
void SCREEN_RefreshScreen(void)
{
    uint8_t   uiPage;
    uint8_t   uiColumn;
    uint16_t  uiLength;
    /* Local slice buffer. Must NOT be the driver's transmit buffer: the copy
     * below and the driver's own copy would overlap and scramble the data. */
    uint8_t   aucSlice[SSD1306_WIDTH];

    /* Nothing changed. */
    if(s_stUpdateArea.uiStartPage > s_stUpdateArea.uiEndPage)
    {
        return;
    }
    /* Clamp, guards against an out-of-range record. */
    if(s_stUpdateArea.uiEndColumn > (SSD1306_WIDTH - 1))
    {
        s_stUpdateArea.uiEndColumn = SSD1306_WIDTH - 1;
    }
    if(s_stUpdateArea.uiEndPage > (SSD1306_PAGES - 1))
    {
        s_stUpdateArea.uiEndPage = SSD1306_PAGES - 1;
    }

    uiLength = (uint16_t)(s_stUpdateArea.uiEndColumn - s_stUpdateArea.uiStartColumn + 1);

    for(uiPage = s_stUpdateArea.uiStartPage; uiPage <= s_stUpdateArea.uiEndPage; uiPage++)
    {
        /* Copy only the dirty column slice of this page into the local buffer. */
        for(uiColumn = s_stUpdateArea.uiStartColumn; uiColumn <= s_stUpdateArea.uiEndColumn; uiColumn++)
        {
            aucSlice[uiColumn] = s_aucDisplayCache[CACHE_INDEX(uiPage, uiColumn)];
        }
        /* Move the panel cursor, then push the whole slice in one I2C transfer. */
        SSD1306_SetPosition(s_stUpdateArea.uiStartColumn, uiPage);
        SSD1306_WriteDataSegment(&aucSlice[s_stUpdateArea.uiStartColumn], uiLength);
    }
    SCREEN_ClearDirtyRecord();
}
