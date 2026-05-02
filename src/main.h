#ifndef LANGTON_ANTS_MAIN_H_
#define LANGTON_ANTS_MAIN_H_

#include "ants.h"
#include "globals.h"

// Main window procedure
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// Initializes app state
bool InitApp(HWND hWnd);

// Creates status bar, and sets initial split and text
bool InitStatusBar(HWND hWnd);

// Re-positions the status bar's tooltip-tool rects against the bar's current
// part rects. Call after every SB_SETPARTS (i.e. each WM_SIZE) so the hover
// targets follow the parts as the window resizes. No-op until InitStatusBar
// has run, and no-op if the tooltip control failed to create.
void LayoutStatusTooltips();

// Closes all windows and cleans up any resources.
void ShutDownApp();

// Shows help
bool LaunchHelp(HWND hWnd);

// About dialog handler
INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif // LANGTON_ANTS_MAIN_H_
