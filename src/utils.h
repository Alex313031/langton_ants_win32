#ifndef LANGTON_ANTS_UTILS_H_
#define LANGTON_ANTS_UTILS_H_

#include "framework.h"

#include <logging.h>

inline const std::wstring sound_file = L"ants.wav"; // Sound to play

// Compile-time switch for how the background music is sourced. When true,
// PlayWavFile ignores its wav_file argument and plays the WAV baked into
// the .exe as the IDR_BGM_WAVE resource (extracted to a temp file so MCI
// can open it — MCI's string API has no "play from memory" form, and we
// need MCI specifically to decode MS ADPCM reliably on Win2K). When false,
// PlayWavFile reads the file by name from the exe directory as before.
// Flip here and rebuild; callers pass this through to PlayWavFile.
inline constexpr bool kUseEmbeddedBgm = true;

extern volatile bool g_playsound;

// Color constants
#define RGB_BLACK   RGB(0, 0, 0)
#define RGB_WHITE   RGB(255, 255, 255)
#define RGB_GREY    RGB(128, 128, 128)
#define RGB_RED     RGB(255, 0, 0)
#define RGB_GREEN   RGB(0, 255, 0)
#define RGB_BLUE    RGB(0, 0, 255)
#define RGB_YELLOW  RGB(255, 255, 0)
#define RGB_CYAN    RGB(0, 255, 255)
#define RGB_MAGENTA RGB(255, 0, 255)

// Time constants
inline constexpr unsigned long kSlowSpeed  = 250UL;
inline constexpr unsigned long kMedSpeed   = 125UL;
inline constexpr unsigned long kHighSpeed  = 33UL;
inline constexpr unsigned long kHyperSpeed = 16UL;
inline constexpr unsigned long kRealTime   = 1UL;

extern unsigned long g_default_speed;

inline constexpr INT CW_WIDTH  = 640;
inline constexpr INT CW_HEIGHT = 640;

inline constexpr INT MINWIDTH  = 192;
inline constexpr INT MINHEIGHT = 192;

extern int g_toolbarHeight; // Height of the top toolbar in pixels; 0 if none. Ants "canvas" lives below it.

// Gets default settings from CHECKED state of menu items
void InitMenuDefaults(HWND hWnd);

// Gets the current side by side directory, regardless of where .exe is started from
const std::wstring GetExeDir();

// Save client area as a .BMP photo, capturing moment menu was clicked.
bool SaveClientBitmap(HWND hWnd);

// Test debug trap
void TestTrap();

// Plays a .wav file. On the first call, opens the file and starts playback
// from position 0. On subsequent calls after a PauseWavFile, issues an MCI
// `resume` so playback continues from where it was paused (no re-open, no
// restart).
//
// If use_embedded is true, wav_file is ignored and the clip is taken from
// the IDR_BGM_WAVE resource baked into the exe. On first play the resource
// is materialized to a temp file (MCI needs a path, not a memory buffer)
// and StopPlayWav deletes that file on cleanup. If use_embedded is false,
// wav_file must name a .wav sitting next to the exe.
bool PlayWavFile(const std::wstring& wav_file, bool use_embedded);

// Pauses the currently-playing sound, preserving playback position. The
// MCI device stays open; a following PlayWavFile will resume rather than
// restart. Used by the mute toolbar / menu toggle.
bool PauseWavFile();

// Fully stops playback and releases the MCI device (stop + close). Used by
// ShutDownApp for cleanup on exit — not by the mute toggle.
bool StopPlayWav();

// Starts and stops playing sound at will.
bool ToggleSound();

// Called by the ants-pause path (TogglePaintAnts) to auto-pause the BGM
// in lockstep with the simulation. If the BGM is currently playing, this
// issues an MCI pause and remembers internally that *we* were the ones
// who paused it. If the BGM is muted / never started, does nothing.
void AntPauseBgm();

// Called by the ants-resume path. Resumes the BGM only if AntPauseBgm
// was the thing that paused it and the user hasn't since toggled sound
// explicitly (ToggleSound clears the internal flag so their choice
// wins over our auto-resume).
void AntResumeBgm();

// Helper functions for MessageBoxW
bool InfoBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

bool WarnBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

bool ErrorBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

// Creates the app's top toolbar as a child of hParent. Call once from
// WM_CREATE. Stores the toolbar handle internally and measures the global
// g_toolbarHeight so the rest of the app can offset the ants area below it.
void CreateAppToolbar(HWND hParent, HINSTANCE hInst);

// Re-auto-sizes the toolbar to the parent's new width and re-measures its
// height into g_toolbarHeight. Call from WM_SIZE. No-op if the toolbar
// hasn't been created yet.
void LayoutToolbar(HWND hWnd);

// Swaps the IDM_PAUSED toolbar button's icon+label between "Pause" (simulation
// running) and "Resume" (simulation paused). Call after toggling g_paused.
void SetPauseButton(bool paused);

// Swaps the IDM_SOUND toolbar button's icon+label between "Music" (silent,
// click to start) and "Mute" (playing, click to stop). Call after ToggleSound.
void SetSoundButton(bool playing);

// Handles a TTN_GETDISPINFOW / TTN_NEEDTEXTW notification from the toolbar's
// tooltip control by supplying a descriptive string based on the hovered
// button's command ID (and, for state-toggling buttons, the current state).
// Call this first inside the parent's WM_NOTIFY and return early if it
// returns true. Returns false for any other notification code so the caller
// can continue dispatching.
bool HandleToolbarTooltips(NMHDR* pnmh);

// Pops up hMenu anchored just below the toolbar button identified by
// idCommand. Useful for WM_COMMAND handlers on TBSTYLE_DROPDOWN buttons
// where the user clicked the button body (not the arrow) and we want to
// show the same dropdown as TBN_DROPDOWN would. hOwner receives the
// WM_COMMAND messages generated by menu selections.
void PopupUnderToolbarButton(HWND hOwner, int idCommand, HMENU hMenu);

#endif // LANGTON_ANTS_UTILS_H_
