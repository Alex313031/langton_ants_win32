#ifndef LANGTON_ANTS_UTILS_H_
#define LANGTON_ANTS_UTILS_H_

// clang-format off
#include "framework.h"
#include <logging.h>
// clang-format on

// Typedef for accessing undocumented RtlGetNtVersionNumbers in ntdll.dll
typedef void(WINAPI* RtlGetNtVersionNumbers_t)(DWORD *pNtMajorVersion,
                                               DWORD *pNtMinorVersion,
                                               DWORD *pNtBuildNumber);

// RtlGetVersion, in NtosKrnl.exe, uses ntdll.dll internally.
// LPOSVERSIONINFOEXW is already a pointer typedef (OSVERSIONINFOEXW*) - no
// extra * here, otherwise the param becomes OSVERSIONINFOEXW** which the
// kernel would try to deref as a pointer to a pointer and corrupt memory.
typedef NTSTATUS(WINAPI* RtlGetVersion_t)(LPOSVERSIONINFOEXW lpVersionInformation);

// User mode function GetVersionExW, but in kernel32.dll. Same pointer-level
// caveat as RtlGetVersion_t above.
typedef BOOL(WINAPI* GetVersionExW_t)(LPOSVERSIONINFOEXW lpVersionInformation);

// Forward-declared from ants.h so this header doesn't have to drag in the
// full ants.h surface just to take an AntAlgorithm parameter.
enum class AntAlgorithm : UINT;

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

// Large and small main application icons
extern HICON kMainIcon;
extern HICON kSmallIcon;

// Default desired ant canvas size (NOT the outer window size). wWinMain
// adds the OS chrome and the toolbar's measured height on top of these
// to compute the actual outer window size, so the user always gets a
// CW_WIDTH x CW_HEIGHT ant canvas at startup regardless of how tall the
// menu bar / toolbar end up being.
inline constexpr INT CW_WIDTH  = 654;
inline constexpr INT CW_HEIGHT = 654;

// Minimum desired ant canvas size. WM_GETMINMAXINFO converts these to
// outer-window minimums (chrome + g_toolbarHeight added) so the canvas
// never gets squeezed below this even when the toolbar wraps onto extra
// rows at narrow widths.
inline constexpr INT MINWIDTH  = 252;
inline constexpr INT MINHEIGHT = 252;

// Width of CPU status area in status bar, for split
inline constexpr INT CPU_STATUS_WIDTH = 120;

// Child window style
inline constexpr DWORD dwCHILD = WS_CHILD | WS_VISIBLE;

// Delay in ms. before resetting status bar text to kDefaultCpuBubbleText.
inline constexpr unsigned long kStatusBarResetDelay = 5000ul; // 5 second delay

extern int g_toolbarHeight; // Height of the top toolbar in pixels; 0 if none. Ants "canvas" lives
                            // below it.

extern int g_statusBarHeight; // Height of the bottom status bar in pixels; 0 if none. Ants
                              // "canvas" ends above it. Measured by InitStatusBar after the bar
                              // is created and re-measured on every WM_SIZE so DPI / font
                              // changes don't leave stale rows of canvas hidden under the bar.

// True between a UserMessage call and the TIMER_STATUS_RESET firing - i.e.
// "the bar is currently showing a transient user message, not the default".
// UserMessage sets it; the WM_TIMER handler for TIMER_STATUS_RESET clears
// it. Read by main.cc on minimize/restore so a pending revert can be
// suspended while minimized and re-armed after restore (with a fresh
// kStatusBarResetDelay window) - the user gets to read the message after
// un-minimizing instead of seeing it blow away while they couldn't.
extern bool g_status_revert_pending;

// Minimum common controls version for certain functions, used for fallback codepaths
// See https://learn.microsoft.com/en-us/windows/win32/controls/common-control-versions
inline constexpr DWORD dwComCtl32TargetVer =
    _PACKVERSION(static_cast<DWORD>(5u), static_cast<DWORD>(82u));

// Gets default settings from CHECKED state of menu items
void InitMenuDefaults(HWND hWnd);

// Gets the current side by side directory, regardless of where .exe is started from
const std::wstring GetExeDir();

// Save client area as a .BMP photo, capturing moment menu was clicked.
bool SaveClientBitmap(HWND hWnd);

// Test debug trap, choosing between DCHECK and CHECK
void TestTrap(const bool dcheck);

// Fills a rect with a solid color. Wraps the CreateSolidBrush + FillRect
// + DeleteObject trio so call sites don't have to repeat all three (and
// can't forget the DeleteObject and leak a GDI brush).
bool FillRectWithColor(HDC hdc, const RECT& rc, COLORREF color);

// Helper functions for MessageBoxW
bool InfoBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

bool WarnBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

bool ErrorBox(HWND hWnd, const std::wstring& title, const std::wstring& message);

// Creates the app's top toolbar as a child of hParent. Call once from
// WM_CREATE. Stores the toolbar handle internally and measures the global
// g_toolbarHeight so the rest of the app can offset the ants area below it.
// Returns true on success; returns false if the toolbar window itself
// failed to create (callers can warn the user that the toolbar will be
// missing - the rest of the app still works via the menu bar).
bool CreateAppToolbar(HWND hParent, HINSTANCE hInst);

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

// Reflects the current ant count on the Num Ants submenu. Pass the value
// you just set g_num_ants to. Counts 1-16 light up the matching IDM_CONC_N
// radio and clear IDM_CONC_CUSTOM; counts outside that range (17-128 from
// the Custom Num dialog, or post-place-mode counts > 16) clear all the
// radios and check IDM_CONC_CUSTOM instead so exactly one entry shows
// a mark.
void SetNumAntsCheck(unsigned int num);

// Reflects the current algorithm on the Settings → Customize → Algorithms
// submenu. Pass the value you just set g_algorithm to. The IDM_CLASSIC..
// IDM_LOGARITHMIC IDs are consecutive and aligned with AntAlgorithm's
// underlying values, so the radio target is derived directly.
void SetAlgorithmCheck(AntAlgorithm algo);

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
// WM_COMMAND messages generated by menu selections. Returns true on
// success; returns false when the toolbar isn't ready, the menu handle
// is null, the button rect lookup failed, or TrackPopupMenu itself
// failed - meaning the user clicked but no menu actually appeared.
bool PopupUnderToolbarButton(HWND hOwner, int idCommand, HMENU hMenu);

// Validates that the input from "Custom Seed" dialog is valid.
// Must be an unsigned integer, no spaces, decimals, or alphanumeric characters.
bool ValidateCustomSeed(LPCWSTR cSeed);

// Validates that the input from "Custom Num Ants" dialog is valid.
// Must be an unsigned integer less than kMaxAntThreads, no spaces, decimals,
// or alphanumeric characters,
bool ValidateCustomNum(LPCWSTR cNum);

// Updates text in a specified part of the status bar.
void UpdateStatusBar(const unsigned int part, const std::wstring& text);

// Logs to console at INFO level, and updates status bar 1st part.
void UserMessage(const std::wstring& message);

// Gets version as human readable wstring.
const std::wstring GetVersionString();

// Gets version as human readable wstring.
const std::wstring GetVersionString();

// Returns APP_NAME as wstring, for easier usage.
const std::wstring GetAppName();

// Returns true on Windows XP (5.1) or later, false on Windows 2000 (5.0)
// or earlier. Used to gate styles / APIs that exist only on WinXP.
bool IsWindowsXpOrLater();

// For checking system's commctl32.dll
bool IsCommCtrlAtLeast(const DWORD to_compare);

// Returns true if the app is running under Wine, false on a real Windows
// kernel. Detection is via ntdll.dll's wine_get_version export, which only
// exists in Wine's ntdll. If outWineVer is non-null AND the result is true,
// the version string is written there. Pass nullptr to skip the version
// (just probe the bool).
bool IsRunningOnWine(std::string* outWineVer);

// Returns the *real* NT kernel version directly out of ntdll.dll, bypassing
// the manifest-driven version shim that GetVersionExW / RtlGetVersion go through.
//
// Why this exists: starting with Windows 8.1, GetVersionExW reports 6.2
// (Win8) for any process that doesn't carry a SupportedOS GUID for the
// later OS in its manifest, regardless of the actual host OS. So an
// unmanifested app running on Win 10 / 11 sees 6.2 from GetVersionExW even
// though the kernel is 10.x. RtlGetNtVersionNumbers (undocumented but
// stable since XP) is *not* shimmed - it returns whatever ntdll really is,
// which is the truth.
//
// Returns false if ntdll.dll isn't loaded or RtlGetNtVersionNumbers
// can't be resolved (Win2K, it was added in XP).
//
// Each out-param is individually optional - pass nullptr for the ones the
// caller doesn't need.
bool GetRawNtVersion(UINT* major, UINT* minor, UINT* build);

// Same as above, but uses RtlGetVersion, also kernel mode but spoofable by user mode
// software with Admin priveleges. Exported from NtosKrnl.exe, but uses ntdll.dll.
// `sp` (optional) receives wServicePackMajor when non-null.
bool GetKernelNtVersion(UINT* major, UINT* minor, UINT* build, UINT* sp = nullptr);

// User mode version of the above, using GetVersionExW. NOTE: On Windows 8.1+
// unmanifested apps can report 6.2, see GetRawNtVersion comment above.
// `sp` (optional) receives wServicePackMajor when non-null.
bool GetUserNtVersion(UINT* major, UINT* minor, UINT* build, UINT* sp = nullptr);

// Logs information about OS version (Wine, and using the above three functions).
void LogOsInfo();

#endif // LANGTON_ANTS_UTILS_H_
