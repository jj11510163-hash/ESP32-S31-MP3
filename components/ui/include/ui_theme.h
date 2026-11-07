#pragma once

/* 128x64 monochrome OLED. Coordinates are pixel based, right/bottom exclusive. */
#define TH_W 128
#define TH_H 64
#define TH_PAGE_MARGIN 8
#define TH_CONTENT_W (TH_W - 2 * TH_PAGE_MARGIN)

#define TH_ROW_H 15
#define TH_MENU_X 4
#define TH_MENU_W (TH_W - 2 * TH_MENU_X)
#define TH_MENU_TOP 2
#define TH_MENU_ROWS 4
#define TH_MENU_PADX 6
#define TH_SCROLLBAR_X 126

#define TH_NP_TITLE_Y 2
#define TH_NP_TITLE_H 20
#define TH_NP_ART_Y 25
#define TH_NP_BAR_Y 43
#define TH_NP_BAR_H 4
#define TH_NP_TIME_Y 49

#define TH_BT_HEADER_Y 1
#define TH_BT_HEADER_H 16
#define TH_BT_ROW_Y 18
#define TH_BT_ROWS 3
