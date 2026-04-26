#ifndef LANGTON_ANTS_ANTS_H
#define LANGTON_ANTS_ANTS_H

#include "framework.h"

extern bool g_monochrome;

// User's ant marker color preference (set from Colors → Ant Colors).
// Set to kRandomAntColor (the default) to roll a per-ant color from
// {magenta, cyan, yellow}; otherwise every ant uses this exact COLORREF.
// Ignored entirely while g_monochrome is true.
inline constexpr COLORREF kRandomAntColor = 0xFFFFFFFFu;
extern COLORREF g_ant_color;

extern volatile UINT g_num_ants;

extern unsigned long g_delay;

// True while the user is "seeding" ants by clicking on the canvas (entered
// via Settings → Custom → Custom Seed, or the IDM_CUSTOM toolbar dropdown).
// While set, WM_LBUTTONDOWN places an ant at the click instead of starting
// a window drag, and the simulation is paused. Cleared either by exiting
// place mode or by ApplyPlacements() at resume time.
extern bool g_place_mode;

// How many ants the user has placed in the current place-mode session.
// Capped at kMaxAntThreads - once it hits the cap, further clicks no-op.
extern int g_placed_ants_count;

// Back buffer for preserving ant paths
extern HDC g_hdcMem;

// Bitmap buffer
extern HBITMAP g_hbmMem;

// Hard upper bound on concurrent ant threads. 8 matches the historical
// Windows 2000 Server/XP+ cores limit. 32 is a reasonable limit for modern machines.
// The IDM_CONC_* menu exposes IDM_CONC_1..IDM_CONC_32 matching this bound.
inline constexpr int kMaxAntThreads = static_cast<int>(32u);

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
DWORD WINAPI AntThread(LPVOID pvoid_in);

// Resizes the ant-thread pool to exactly `targetCount` live threads, spawning
// new ones or terminating excess ones as needed. Clamps to [1, kMaxAntThreads].
// Called internally from ShowAnts and SetNumAnts; safe to call repeatedly.
bool EnsureThreadCount(int targetCount);

// Releases one tick to every currently-active ant thread (SetEvent on each
// slot's private event). Call from WM_TIMER and wherever else a one-shot
// draw pulse is wanted (e.g. IDM_SINGLE, resume-from-pause).
void SignalAntsTick();

// Requests that every active ant reroll its position, direction, and
// marker color on its next tick. With pulse = true (the default, used by
// IDM_REPAINT) the tick events are signalled so the reseed runs even
// while paused - that's what makes "Repaint now" repaint immediately.
// With pulse = false (IDM_STOP) the flag is set but no event fires, so
// the threads stay parked and the canvas stays blank until the user
// resumes from pause.
void ReseedAnts(bool pulse = true);

// Refreshes each running ant's marker color against the current
// g_monochrome value (mono → match the trail color, otherwise pick from
// magenta/cyan/yellow). Position, direction and onBg are preserved so
// the simulation continues exactly where it was - used by IDM_MONOCHROME
// so toggling mono behaves like picking a Colors entry (just swaps colors).
void RefreshAntColors();

// Use a custom seed for the ants. Returns true on success, false if the
// internal teardown + respawn couldn't bring the thread pool back up
// (CreateThread / CreateEvent failure inside EnsureThreadCount). Also
// returns false if no ants are currently running, so there's nothing
// to seed.
bool CustomSeedAnts(const unsigned int custom_seed);

// Terminates all ant threads and closes their events/the timer. Called from
// WM_DESTROY; safe to call more than once.
void ShutdownAnts();

// For handling back buffer bitmap for smooth resize. Returns true on
// success (including the no-op fast path where the existing bitmap
// already matches), false on bad inputs (cx/cy <= 0, no g_hdcMem) or
// CreateCompatibleBitmap failure.
bool RecreateBackBuffer(HWND hWnd, int cx, int cy);

// Swaps every pixel in the back buffer that currently equals oldColor over to
// newColor, leaving all other (ant path) pixels untouched. Used by the background
// colour menu so the bg can change without erasing the ant paths already painted.
void RecolorBackground(COLORREF oldColor, COLORREF newColor);

// Sets the active ant count (clamped to [1, kMaxAntThreads]). Returns true
// on success; returns false if the pool was running and the resulting
// EnsureThreadCount call couldn't grow / shrink the thread pool to the
// requested size.
bool SetNumAnts(const unsigned int num);

// Starts drawing ant(s) path(s) on client area
bool ShowAnts();

// Pauses/resumes painting, for i.e. taking a snapshot, or showing a friend the current state.
// Returns true on success; returns false on a null hWnd, or when the
// resume path's SetTimer call fails (the timer can't be re-armed and
// the simulation will sit idle even though g_paused was cleared).
bool TogglePaintAnts(HWND hWnd);

// Enters "place ants" mode: clears any pending placement list and sets
// g_place_mode = true. Caller is responsible for ensuring the simulation
// is paused (and clearing the canvas, if desired) before calling.
void EnterPlaceMode();

// Exits place mode without applying any pending placements. Used when the
// user toggles Custom Seed off, or hits Repaint Now mid-placement.
void ExitPlaceMode();

// Places an ant at the given window-client coordinates if in place mode and
// not already at kMaxAntThreads. Paints the ant marker (random pick from
// {magenta, cyan, yellow}) into the back buffer, samples the underlying
// pixel to record onBg, and appends to the placement list. Returns true if
// a placement was made.
bool PlaceAntAtClient(int clientX, int clientY);

// Pops the most-recently-placed ant off the list and erases its marker
// from the back buffer. Wired to the Ctrl+Z accelerator (IDM_UNDO);
// only does anything in place mode. Returns true if a placement was
// undone, false when not in place mode or the list is already empty.
bool UndoLastPlacement();

// For "Custom Seed" dialog box
INT_PTR CALLBACK CustomDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

#endif // LANGTON_ANTS_ANTS_H
