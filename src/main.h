#ifndef LANGTON_ANTS_MAIN_H_
#define LANGTON_ANTS_MAIN_H_

#include "ants.h"
#include "globals.h"

// Main window procedure
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

// Initializes app state
bool InitApp(HWND hWnd);

// Closes all windows and cleans up any resources.
void ShutDownApp();

// Shows help
bool LaunchHelp(HWND hWnd);

// About dialog handler
INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif // LANGTON_ANTS_MAIN_H_
