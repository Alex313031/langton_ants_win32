#ifndef LANGTON_ANTS_GLOBALS_H_
#define LANGTON_ANTS_GLOBALS_H_

#include "framework.h"

// Main client width/height
extern int cxClient;
extern int cyClient;

extern HWND mainHwnd; // Our main window handle

extern volatile bool g_running; // Controlling ants threads state
extern volatile bool g_paused;  // Keep track of paused state. PauseAnts() uses this in utils.cc

extern CRITICAL_SECTION g_paintCS; // For thread sync on back buffer access

extern COLORREF g_bkg_color; // Current background color, changed via the Background Color menu

#endif // LANGTON_ANTS_GLOBALS_H_
