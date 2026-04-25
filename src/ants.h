#ifndef LANGTON_ANTS_ANTS_H
#define LANGTON_ANTS_ANTS_H

#include "framework.h"

extern bool g_monochrome;

extern volatile UINT g_num_ants;

extern unsigned long g_delay;

// Back buffer for preserving ant paths
extern HDC g_hdcMem;

// Bitmap buffer
extern HBITMAP g_hbmMem;

// Hard upper bound on concurrent ant threads. 8 matches the historical
// Windows 2000 Server / Windows XP CPU-license limit — beyond that on a
// weak 1-core box the drawing threads would just thrash the scheduler.
// The IDM_CONC_* menu exposes IDM_CONC_1..IDM_CONC_8 matching this bound.
#define kMaxAntThreads 8

// Size of one logical ant "pixel" in real hardware pixels. An ant occupies
// a CELL_PX × CELL_PX square and every path mark quantizes to the same
// grid, so all coordinates in the automaton are cell-indices and only this
// constant converts back to the back buffer's pixel space. 6 keeps trails
// clearly visible on typical displays without over-coarsening the canvas.
inline constexpr int CELL_PX = 6;

// One ant thread. Each thread waits on its own private auto-reset event and
// updates ant location once per tick. The ant-thread pool can be grown or shrunk at
// runtime (see EnsureThreadCount) so we never have more threads alive than
// the user asked for via the Num Ants menu.
DWORD WINAPI AntThread(LPVOID pvoid);

// Resizes the ant-thread pool to exactly `targetCount` live threads, spawning
// new ones or terminating excess ones as needed. Clamps to [1, kMaxAntThreads].
// Called internally from ShowAnts and SetNumAnts; safe to call repeatedly.
bool EnsureThreadCount(int targetCount);

// Releases one tick to every currently-active ant thread (SetEvent on each
// slot's private event). Call from WM_TIMER and wherever else a one-shot
// draw pulse is wanted (e.g. IDM_SINGLE, resume-from-pause).
void SignalAntsTick();

// Requests that every active ant reroll its position, direction, and
// marker color on its next tick — used by IDM_REPAINT so "Repaint now"
// both clears the canvas AND restarts the ants from fresh random spots.
// Also pulses the tick events so the reseed actually runs even when
// the simulation is currently paused.
void ReseedAnts();

// Terminates all ant threads and closes their events/the timer. Called from
// WM_DESTROY; safe to call more than once.
void ShutdownAnts();

// For handling back buffer bitmap for smooth resize
void RecreateBackBuffer(HWND hWnd, int cx, int cy);

// Swaps every pixel in the back buffer that currently equals oldColor over to
// newColor, leaving all other (ant path) pixels untouched. Used by the background
// colour menu so the bg can change without erasing the ant paths already painted.
void RecolorBackground(COLORREF oldColor, COLORREF newColor);

void SetNumAnts(const unsigned int num);

// Starts drawing ant(s) path(s) on client area
bool ShowAnts();

// Pauses/resumes painting, for i.e. taking a snapshot, or showing a friend the current state.
void TogglePaintAnts(HWND hWnd);

#endif // LANGTON_ANTS_ANTS_H
