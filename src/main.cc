/*------------------------------------------
   Langton's Ants — Win32 GDI implementation
   Copyright (c) 2026 Alex313031
  ------------------------------------------*/

#include "main.h"

#include "resource.h"
#include "sound.h"
#include "utils.h"
#include "version.h"

HWND mainHwnd = nullptr;

HINSTANCE g_hInstance = nullptr;

int cxClient = 0;
int cyClient = 0;

static bool s_resizing = false;
static POINT s_resizeOrigin = {};
static SIZE s_resizeStartSize = {};

// Tracks whether the last WM_SIZE minimized the window. Set by WM_SIZE on
// SIZE_MINIMIZED, cleared by the next non-minimize WM_SIZE. Used to decide
// whether the just-arrived size event is "we're coming back from a
// minimize" (restart the tick source) vs. a normal resize (no-op).
static bool s_was_minimized = false;

HDC g_hdcMem     = nullptr;
HBITMAP g_hbmMem = nullptr;

// CRITICAL_SECTION is a lightweight Win32 synchronization primitive for mutual
// exclusion between threads on the same process. Unlike a mutex, it cannot be
// shared across processes, but is faster for intra-process use. We use it to
// prevent the ant thread and the main thread from accessing the back buffer
// (g_hdcMem / g_hbmMem) at the same time.
CRITICAL_SECTION g_paintCS;

// Current background color. Defaults to black, changed via the Background
// Color menu. Used when filling the back buffer on resize and on WM_PAINT.
COLORREF g_bkg_color = RGB_BLACK;

// Whether to open conhost window for debugging.
static constexpr bool debug_console = is_debug;

int APIENTRY wWinMain(HINSTANCE hInstance,
                      HINSTANCE hPrevInstance,
                      LPWSTR lpCmdLine,
                      int iCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  g_hInstance = hInstance;
  // Initialize common controls
  INITCOMMONCONTROLSEX icex;
  icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  icex.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
  InitCommonControlsEx(&icex);

  static const LPCWSTR appTitle = APP_NAME;
  static const LPCWSTR szClassName = MAIN_WNDCLASS;

  WNDCLASSEXW wndclass;
  wndclass.cbSize        = sizeof(WNDCLASSEX);
  wndclass.style         = 0;
  wndclass.lpfnWndProc   = WindowProc;
  wndclass.cbClsExtra    = 0;
  wndclass.cbWndExtra    = 0;
  wndclass.hInstance     = hInstance;
  wndclass.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));
  wndclass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
  // No stock brush matches our default bg (there's only black / white /
  // grey / null), and we handle erase + paint ourselves — WM_ERASEBKGND
  // returns TRUE and WM_PAINT fills with g_bkg_color. nullptr here skips
  // the OS's pre-fill entirely so there's no flash of a wrong-colored
  // window before our first WM_PAINT.
  wndclass.hbrBackground = nullptr;
  wndclass.lpszMenuName  = MAKEINTRESOURCEW(IDR_MAIN);
  wndclass.lpszClassName = szClassName;
  wndclass.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_SMALL));

  if (!RegisterClassExW(&wndclass)) {
    ErrorBox(nullptr, L"RegisterClassEx Error", L"This program requires Windows NT!");
    return 2;
  } else {
    // Set up our logging using mini_logger library.
    const logging::LogDest kLogSink = debug_console ? logging::LOG_TO_ALL : logging::LOG_NONE;
    const std::wstring kLogFile(L"langton_ants.log");
    logging::LogInitSettings LoggingSettings;
    LoggingSettings.log_sink          = kLogSink;
    LoggingSettings.logfile_name      = kLogFile;
    LoggingSettings.app_name          = appTitle;
    LoggingSettings.show_func_sigs    = false;
    LoggingSettings.show_line_numbers = false;
    LoggingSettings.show_time         = false;
    LoggingSettings.full_prefix_level = LOG_ERROR;
    const bool init_logging           = logging::InitLogging(g_hInstance, LoggingSettings);
    if (init_logging) {
      logging::SetIsDCheck(is_dcheck);
      LOG(INFO) << L"---- Welcome to Langton's Ants Win32 ----";
    } else {
      ErrorBox(nullptr, L"Logging Initialization Failure", L"InitLogging failed!");
      return 3;
    }
  }

  InitializeCriticalSection(&g_paintCS);
  // Tighten the system timer resolution from the default ~15.6ms down
  // to 1ms for the life of the process. SetTimer (driving the ant tick)
  // and the sound subsystem both benefit — at Hyper / Realtime speeds
  // the default resolution rounds our requested interval up to the
  // nearest 15ms, so misses-and-catches-up visibly as stutter. Paired
  // with timeEndPeriod below.
  timeBeginPeriod(1);
  // Spin up the BGM worker thread. The worker owns the MCI device and
  // its hidden notify window, so all mciSendString calls (including
  // the 2-second loop re-issue) run off the main thread. Must come
  // before any PlayWavFile call; PostMessageW(WM_APP_AUTOPLAY) from
  // InitApp fires only after the message loop starts, well after this.
  InitBgm();

  static constexpr DWORD exStyle =
#if _WIN32_WINNT > 0x0602 // Only Windows 8.1+ handles composited correctly with the way this app works.
      WS_EX_OVERLAPPEDWINDOW | WS_EX_COMPOSITED;
#else
      WS_EX_OVERLAPPEDWINDOW;
#endif
  // WS_CLIPCHILDREN keeps the parent's painting out of child windows' regions
  // (here: the toolbar). The toolbar is responsible for drawing itself; the
  // OS handles its theming (themed on XP+, classic on Win2000).
  static constexpr DWORD style =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX | WS_CLIPCHILDREN;
  // Center on the primary monitor's work area (the screen minus the
  // taskbar). SPI_GETWORKAREA is available back to Win2000 and handles
  // the case where the taskbar is docked at the top/left/right. If the
  // query fails for any reason, fall back to CW_USEDEFAULT so the OS
  // places the window wherever it likes rather than at (0,0) with a
  // half-off-screen origin.
  int xPos = CW_USEDEFAULT;
  int yPos = CW_USEDEFAULT;
  RECT workArea = {};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
    xPos = workArea.left + ((workArea.right  - workArea.left) - CW_WIDTH)  / 2;
    yPos = workArea.top  + ((workArea.bottom - workArea.top)  - CW_HEIGHT) / 2;
  }
  mainHwnd = CreateWindowExW(exStyle, szClassName, appTitle, style,
                         xPos, yPos,
                         CW_WIDTH, CW_HEIGHT, nullptr, nullptr, hInstance, nullptr);

  if (mainHwnd == nullptr) {
    return 1;
  }
  ShowWindow(mainHwnd, iCmdShow);
  if (!UpdateWindow(mainHwnd)) {
    return 1;
  }

  HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_MAIN));

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!TranslateAcceleratorW(mainHwnd, hAccel, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (hAccel != nullptr) {
    DestroyAcceleratorTable(hAccel);
  }
  DeleteCriticalSection(&g_paintCS);
  // Match the timeBeginPeriod at startup. Not strictly needed (the OS
  // drops the requested resolution when the process exits) but good
  // hygiene — especially on older Windows where the effect is system-
  // wide rather than per-process.
  timeEndPeriod(1);
  return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE:
      if (mainHwnd == nullptr) {
        mainHwnd = hWnd; // Prevent race condition in InitApp
      }
      // CreateCompatibleDC(nullptr) creates an off-screen memory DC compatible
      // with the screen. At this point it holds a 1x1 monochrome placeholder
      // bitmap; RecreateBackBuffer (called on the first WM_SIZE) replaces it
      // with a full-size bitmap matched to the window.
      g_hdcMem = CreateCompatibleDC(nullptr);
      // Build the toolbar before InitMenuDefaults so any future toolbar-driven
      // default reading could work, and before InitApp so cyClient computed
      // in the first WM_SIZE already excludes the toolbar height.
      // CreateAppToolbar (utils.cc) stores the handle internally and sets
      // g_toolbarHeight; we don't need the handle here.
      CreateAppToolbar(hWnd, g_hInstance);
      InitMenuDefaults(hWnd);
      InitApp(hWnd);
      break;
    case WM_TIMER:
      // WM_TIMER fires on the main thread at the interval set by SetTimer.
      // We signal every active ant thread's tick event rather than drawing
      // here directly, keeping all GDI work on the ant threads and leaving
      // the main thread free to process input and paint messages.
      if (wParam == TIMER_ANTS) {
        SignalAntsTick();
      }
      break;
    case WM_APP_AUTOPLAY: {
      // Deferred startup auto-play. InitApp (called from WM_CREATE) posts
      // this message rather than calling SyncBgm directly so the MCI work
      // runs in the normal WindowProc dispatch context — defensive against
      // older Windows quirks with audio APIs invoked inside WM_CREATE.
      // SyncBgm reads g_playsound (seeded from the IDM_SOUND menu state in
      // InitMenuDefaults) and starts playback if the user wants sound.
      // If the open/play fails, drop the user pref to false and uncheck
      // the menu so the UI doesn't keep promising audio that never plays.
      if (!SyncBgm()) {
        g_playsound = false;
        HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
        CheckMenuItem(hSettings, IDM_SOUND, MF_BYCOMMAND | MF_UNCHECKED);
      }
      SetSoundButton(g_playsound);
      break;
    }
    case WM_ERASEBKGND:
      // Returning TRUE tells Windows we have handled background erasing
      // ourselves, suppressing the default white fill. We do our own filling
      // in WM_PAINT so the two operations don't race or double-paint.
      return TRUE;
    case WM_GETMINMAXINFO: {
      // Set the minimum size for the window
      LPMINMAXINFO pMinMaxInfo = reinterpret_cast<LPMINMAXINFO>(lParam);
      const int MAXWIDTH  = GetSystemMetrics(SM_CXMAXIMIZED);
      const int MAXHEIGHT = GetSystemMetrics(SM_CYMAXIMIZED);
      pMinMaxInfo->ptMinTrackSize.x = MINWIDTH;
      pMinMaxInfo->ptMinTrackSize.y = MINHEIGHT;
      pMinMaxInfo->ptMaxTrackSize.x = MAXWIDTH;
      pMinMaxInfo->ptMaxTrackSize.y = MAXHEIGHT;
      break;
    }
    case WM_PAINT: {
      // BeginPaint validates the dirty region and fills ps.rcPaint with the
      // bounding rect of the area Windows wants us to repaint. Nothing is
      // drawn to the screen until EndPaint is called.
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hWnd, &ps);
      // Fill the dirty region with the background color. Covers newly exposed
      // pixels during resize and startup before the back buffer is ready.
      // WS_CLIPCHILDREN excludes the toolbar's rect automatically, so this
      // fill never touches the toolbar area.
      HBRUSH hBkgBrush = CreateSolidBrush(g_bkg_color);
      FillRect(hdc, &ps.rcPaint, hBkgBrush);
      DeleteObject(hBkgBrush);
      // Hold the lock so the ant thread cannot replace the back buffer
      // bitmap between our null-check and the BitBlt call.
      EnterCriticalSection(&g_paintCS);
      if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
        // Blit the whole back buffer to the window at (0, g_toolbarHeight).
        // Back buffer coords are ants-canvas-local (0..cxClient-1, 0..cyClient-1);
        // shifting by the toolbar height places the canvas below the toolbar.
        BitBlt(hdc, 0, g_toolbarHeight, cxClient, cyClient,
               g_hdcMem, 0, 0, SRCCOPY);
      }
      LeaveCriticalSection(&g_paintCS);
      EndPaint(hWnd, &ps);
      break;
    }
    case WM_SIZE: {
      if (wParam == SIZE_MINIMIZED) {
        // Minimize freezes the simulation: we kill the tick source so the
        // ant threads park on their wait events with zero CPU use. We
        // deliberately don't touch cxClient / cyClient or the back buffer —
        // the bitmap keeps holding the canvas, and each ant's thread-local
        // cellX / cellY / dir / onBg state survives untouched — so when
        // restore fires we come back exactly where we left off, trails
        // and all.
        s_was_minimized = true;
        KillTimer(hWnd, TIMER_ANTS);
        break;
      }
      // Let the toolbar re-fit the new parent width and re-measure its height.
      // All toolbar state lives in utils.cc; this one call updates
      // g_toolbarHeight as needed.
      LayoutToolbar(hWnd);
      // cxClient / cyClient represent the ants canvas area, not the parent's client
      // area — the toolbar isn't drawable space. Clamp to zero when the
      // window is smaller than the toolbar (extreme resize).
      cxClient = LOWORD(lParam);
      cyClient = HIWORD(lParam) - g_toolbarHeight;
      if (cyClient < 0) cyClient = 0;
      // The ants canvas changed size, so recreate the back buffer to match.
      // If it grew, the old bitmap would be too small and BitBlt would read
      // outside its bounds; if it shrank, the old one just wastes memory.
      // When restoring from minimize the dimensions match the existing
      // bitmap and RecreateBackBuffer's fast-path keeps the trails alive.
      RecreateBackBuffer(hWnd, cxClient, cyClient);
      if (s_was_minimized) {
        s_was_minimized = false;
        // Restored from minimize — bring the tick source back unless the
        // user had explicitly paused before minimizing (g_paused still
        // reflects that choice, so we honor it).
        if (!g_paused) {
          SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr);
        }
      }
      break;
    }
    case WM_NOTIFY: {
      // Toolbar dropdown buttons (TBSTYLE_DROPDOWN + TBSTYLE_EX_DRAWDDARROWS)
      // send TBN_DROPDOWN when the user clicks the arrow. We handle it for
      // IDM_ANTS and IDM_SPEED by popping up the corresponding Settings
      // submenu anchored under the button. lParam points to an NMTOOLBAR
      // whose rcButton is in toolbar-client coords — ClientToScreen on the
      // toolbar's HWND (hdr.hwndFrom) gives the screen-coord anchor
      // TrackPopupMenu wants.
      LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
      // Toolbar tooltip requests are cheap and noisy, so let utils.cc answer
      // them first and only fall through to other toolbar notifications when
      // it wasn't a tooltip message.
      if (HandleToolbarTooltips(pnmh)) break;
      if (pnmh->code == TBN_DROPDOWN) {
        LPNMTOOLBAR pnmtb = reinterpret_cast<LPNMTOOLBAR>(lParam);
        // Anchor whatever popup we show just below the button's bottom edge.
        POINT pt = { pnmtb->rcButton.left, pnmtb->rcButton.bottom };
        ClientToScreen(pnmh->hwndFrom, &pt);

        // Reusing the live HMENU from the main menu bar means radio check
        // marks stay in perfect sync with the existing IDM_CONC_* /
        // IDM_SLOW..IDM_HYPER handlers — no duplicate items, no manual
        // state sync needed.
        if (pnmtb->iItem == IDM_ANTS) {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hAnts     = GetSubMenu(hSettings, 3);
          TrackPopupMenu(hAnts, TPM_LEFTALIGN | TPM_TOPALIGN,
                         pt.x, pt.y, 0, hWnd, nullptr);
          return TBDDRET_DEFAULT;
        }
        if (pnmtb->iItem == IDM_SPEED) {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hSpeed    = GetSubMenu(hSettings, 5);
          TrackPopupMenu(hSpeed, TPM_LEFTALIGN | TPM_TOPALIGN,
                         pt.x, pt.y, 0, hWnd, nullptr);
          return TBDDRET_DEFAULT;
        }
        if (pnmtb->iItem == IDM_CUSTOM) {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hCustom   = GetSubMenu(hSettings, 9);
          TrackPopupMenu(hCustom, TPM_LEFTALIGN | TPM_TOPALIGN,
                         pt.x, pt.y, 0, hWnd, nullptr);
          return TBDDRET_DEFAULT;
        }
      }
      break;
    }
    case WM_COMMAND: {
      const int command = LOWORD(wParam);
      switch (command) {
        case IDM_EXIT:
          ShutDownApp();
          break;
        case IDM_ABOUT:
          if (!g_playsound) {
            PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC);
          }
          DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_ABOUTDLG), hWnd, AboutDlgProc);
          break;
        case IDM_HELP:
          LaunchHelp(hWnd);
          break;
        case IDM_SAVE_AS:
          SaveClientBitmap(hWnd);
          break;
        case IDM_ANTS: {
          // Button-body click on the Num Ants split button. Show the same
          // dropdown the arrow does so users don't have to hit the arrow's
          // narrow hit box.
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hAnts     = GetSubMenu(hSettings, 3);
          PopupUnderToolbarButton(hWnd, IDM_ANTS, hAnts);
          break;
        }
        case IDM_SPEED: {
          // Button-body click on the Speed split button — mirrors IDM_ANTS.
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hSpeed    = GetSubMenu(hSettings, 5);
          PopupUnderToolbarButton(hWnd, IDM_SPEED, hSpeed);
          break;
        }
        case IDM_CUSTOM: {
          // Button-body click on the Custom split button — mirrors IDM_ANTS.
          // Place-ant mode lives on its own menu item now (IDM_CUSTOMPLACE).
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hCustom   = GetSubMenu(hSettings, 9);
          PopupUnderToolbarButton(hWnd, IDM_CUSTOM, hCustom);
          break;
        }
        case IDM_CUSTOMPLACE: {
          // Always re-enters place mode from a clean slate: pause the sim
          // and set g_stopped = true so the toolbar's pause/play button
          // says "Play" (rather than "Resume") after the user is done
          // placing, wipe the canvas, and reset the placement list —
          // discarding any ants dropped during a prior, un-resumed
          // placement session. Audio follows the pause via SyncBgm.
          if (!g_paused) {
            TogglePaintAnts(hWnd);
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
            CheckMenuItem(hSettings, IDM_PAUSED, MF_BYCOMMAND | MF_CHECKED);
          }
          // Refresh the pause/play button label unconditionally — when
          // entering place mode while already paused, the !g_paused
          // branch above didn't run and the label would otherwise stay
          // at "Resume" instead of flipping to "Play".
          g_stopped = true;
          SetPauseButton(g_paused);
          // Audio follows the simulation automatically: TogglePaintAnts
          // calls SyncBgm, which pauses the BGM whenever ants aren't
          // running. The next Play resumes it through the same call.
          EnterCriticalSection(&g_paintCS);
          if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
            RECT rc = { 0, 0, cxClient, cyClient };
            HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
            FillRect(g_hdcMem, &rc, hBrush);
            DeleteObject(hBrush);
          }
          LeaveCriticalSection(&g_paintCS);
          EnterPlaceMode();
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_CUSTOMSEED: {
          DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_CUSTOMDLG), hWnd, CustomDlgProc);
          break;
        }
        case IDM_SOUND: {
          if (ToggleSound()) {
            // Only update check state if toggling sound on/off succeeded.
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
            CheckMenuItem(hSettings, IDM_SOUND,
                          MF_BYCOMMAND | (g_playsound ? MF_CHECKED : MF_UNCHECKED));
            // Mirror the state on the toolbar: swap icon + label.
            SetSoundButton(g_playsound);
          }
          break;
        }
        case IDM_PAUSED: {
          // If we're about to resume out of place mode the placements get
          // drained inside TogglePaintAnts and g_num_ants may shrink to the
          // placed count — track that here so we can refresh the Num Ants
          // radio after the toggle.
          const bool drainedPlacements = (g_paused && g_place_mode);
          TogglePaintAnts(hWnd);
          // Reflect the new paused state in the menu check mark.
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          CheckMenuItem(hSettings, IDM_PAUSED,
                        MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
          // Mirror the state on the toolbar: swap icon + label.
          SetPauseButton(g_paused);
          if (drainedPlacements && g_num_ants >= 1 && g_num_ants <= kMaxAntThreads) {
            HMENU hConc = GetSubMenu(hSettings, 3);
            CheckMenuRadioItem(hConc, IDM_CONC_1, IDM_CONC_32,
                               IDM_CONC_1 + (g_num_ants - 1), MF_BYCOMMAND);
          }
          break;
        }
        case IDM_STOP: {
          // Halt the simulation, wipe the canvas, and reseed the threads so a
          // subsequent Resume starts from fresh random positions — "ready to
          // start new ants with new settings". Pause via TogglePaintAnts so
          // the timer + BGM are quieted in the same code path the user gets
          // from the Pause button. Place-mode and any pending placements are
          // discarded too, since a Stop is a clean-slate intent.
          if (!g_paused) {
            TogglePaintAnts(hWnd);
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
            CheckMenuItem(hSettings, IDM_PAUSED, MF_BYCOMMAND | MF_CHECKED);
          }
          if (g_place_mode) ExitPlaceMode();
          // Mark the simulation as stopped (fresh state) so the toolbar's
          // pause/play button switches its label from "Resume" to "Play".
          // Refresh unconditionally — when the user hits Stop while already
          // paused, the !g_paused branch above didn't run SetPauseButton
          // and the label would otherwise stay at the old "Resume".
          g_stopped = true;
          SetPauseButton(g_paused);
          // Audio follows the simulation automatically: TogglePaintAnts
          // above (when called) pauses BGM via SyncBgm. The user's sound
          // preference (g_playsound) is preserved across Stop, so a later
          // Play will resume audio if it was on before.
          EnterCriticalSection(&g_paintCS);
          if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
            RECT rc = { 0, 0, cxClient, cyClient };
            HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
            FillRect(g_hdcMem, &rc, hBrush);
            DeleteObject(hBrush);
          }
          LeaveCriticalSection(&g_paintCS);
          // pulse=false: just flag the reseed without waking the parked
          // threads. A pulse here would wake them despite being paused
          // and immediately re-paint fresh ant markers on the just-wiped
          // canvas. We want the canvas to stay blank until the user
          // presses play; the resume path in TogglePaintAnts will pulse
          // the threads then.
          ReseedAnts(false);
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_SINGLE: {
          // Single-step the canvas. On first press we transition into the
          // paused state (KillTimer + check IDM_PAUSED) so the user can see
          // they're frozen; every press after that just pulses the draw event
          // once, giving the ant thread one iteration before it blocks again.
          //
          // To exit single-step mode the user presses IDM_PAUSED, which flips
          // g_paused back to false and re-arms the timer — normal operation
          // resumes with no extra logic needed here.
          if (!g_paused) {
            TogglePaintAnts(hWnd); // toggles g_paused=true and KillTimer
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
            CheckMenuItem(hSettings, IDM_PAUSED, MF_BYCOMMAND | MF_CHECKED);
            // Mirror the paused state onto the toolbar's Pause/Resume
            // button so icon + label match the menu check mark.
            SetPauseButton(g_paused);
          }
          // Pulse every active ant thread's tick event once. The timer is
          // off (paused), so this is the only source of ticks — each thread
          // wakes, moves its ant by one space.
          SignalAntsTick();
          break;
        }
        case IDM_REPAINT: {
          // Clear the back buffer to the current background color, then
          // reseed every ant so their positions, directions, and marker
          // colors all reroll on the next tick. All user settings (speed,
          // num ants, monochrome, etc.) stay intact — only the runtime
          // per-ant state resets. If the user was mid-placement, abandon it
          // — REPAINT and Custom Seed are mutually exclusive intents.
          if (g_place_mode) ExitPlaceMode();
          EnterCriticalSection(&g_paintCS);
          if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
            RECT rc = { 0, 0, cxClient, cyClient };
            HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
            FillRect(g_hdcMem, &rc, hBrush);
            DeleteObject(hBrush);
          }
          LeaveCriticalSection(&g_paintCS);
          ReseedAnts();
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_CONC_1:
        case IDM_CONC_2:
        case IDM_CONC_3:
        case IDM_CONC_4:
        case IDM_CONC_5:
        case IDM_CONC_6:
        case IDM_CONC_7:
        case IDM_CONC_8:
        case IDM_CONC_9:
        case IDM_CONC_10:
        case IDM_CONC_11:
        case IDM_CONC_12:
        case IDM_CONC_13:
        case IDM_CONC_14:
        case IDM_CONC_15:
        case IDM_CONC_16:
        case IDM_CONC_17:
        case IDM_CONC_18:
        case IDM_CONC_19:
        case IDM_CONC_20:
        case IDM_CONC_21:
        case IDM_CONC_22:
        case IDM_CONC_23:
        case IDM_CONC_24:
        case IDM_CONC_25:
        case IDM_CONC_26:
        case IDM_CONC_27:
        case IDM_CONC_28:
        case IDM_CONC_29:
        case IDM_CONC_30:
        case IDM_CONC_31:
        case IDM_CONC_32: {
          // Consecutive IDs let us derive the count directly from the command.
          SetNumAnts((command - IDM_CONC_1) + 1);
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hConc     = GetSubMenu(hSettings, 3);
          CheckMenuRadioItem(hConc, IDM_CONC_1, IDM_CONC_32, command, MF_BYCOMMAND);
          break;
        }
        case IDM_MONOCHROME: {
          g_monochrome = !g_monochrome;
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          // Toggle the check mark on the menu item to show current state.
          CheckMenuItem(hSettings, IDM_MONOCHROME,
                        MF_BYCOMMAND | (g_monochrome ? MF_CHECKED : MF_UNCHECKED));
          // Grey out or restore the chromatic bg options. White, black,
          // and grey all count as monochrome, so only the R/G/B entries
          // get disabled.
          HMENU hBkgMenu = GetSubMenu(hSettings, 8);
          const UINT colorState = g_monochrome ? MF_GRAYED : MF_ENABLED;
          EnableMenuItem(hBkgMenu, IDM_RED_BKG,   MF_BYCOMMAND | colorState);
          EnableMenuItem(hBkgMenu, IDM_GREEN_BKG, MF_BYCOMMAND | colorState);
          EnableMenuItem(hBkgMenu, IDM_BLUE_BKG,  MF_BYCOMMAND | colorState);
          // Entering monochrome snaps the bg to grey (ant color then
          // becomes white automatically via CurrentPathColor). The user
          // can still switch to white or black manually after the fact.
          // Swap bg pixels in place via RecolorBackground rather than
          // clearing the canvas — same pattern as the background-color
          // menu, so existing ant trails are preserved.
          if (g_monochrome && g_bkg_color != RGB_GREY) {
            const COLORREF oldBg = g_bkg_color;
            g_bkg_color = RGB_GREY;
            CheckMenuRadioItem(hBkgMenu, IDM_WHITE_BKG, IDM_BLUE_BKG, IDM_GREY_BKG, MF_BYCOMMAND);
            RecolorBackground(oldBg, g_bkg_color);
          }
          // Refresh each running ant's cached antColor against the new
          // g_monochrome — color-only, no position/dir/onBg touched, so
          // the simulation continues exactly where it was. RefreshAntColors
          // pulses the tick events so the new colors show up immediately
          // even while paused.
          RefreshAntColors();
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_WHITE_BKG:
        case IDM_BLACK_BKG:
        case IDM_GREY_BKG:
        case IDM_RED_BKG:
        case IDM_GREEN_BKG:
        case IDM_BLUE_BKG: {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hBkgMenu  = GetSubMenu(hSettings, 8);
          CheckMenuRadioItem(hBkgMenu, IDM_WHITE_BKG, IDM_BLUE_BKG, command, MF_BYCOMMAND);
          const COLORREF oldColor = g_bkg_color;
          switch (command) {
            case IDM_WHITE_BKG: g_bkg_color = RGB_WHITE; break;
            case IDM_BLACK_BKG: g_bkg_color = RGB_BLACK; break;
            case IDM_GREY_BKG:  g_bkg_color = RGB_GREY; break;
            case IDM_RED_BKG:   g_bkg_color = RGB_RED;   break;
            case IDM_GREEN_BKG: g_bkg_color = RGB_GREEN; break;
            default:            g_bkg_color = RGB_BLUE;  break;
          }
          // Swap only the old background pixels over to the new color. Ant path
          // pixels are left untouched, so existing ants paths are preserved across
          // background changes.
          RecolorBackground(oldColor, g_bkg_color);
          // Invalidate the whole client area so WM_PAINT blits the updated
          // back buffer to the screen. FALSE = do not erase background first
          // (we handle that in WM_PAINT ourselves).
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_SLOW:
        case IDM_MEDIUM:
        case IDM_FAST:
        case IDM_HYPER:
        case IDM_REALTIME: {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
          HMENU hDelay    = GetSubMenu(hSettings, 5);
          CheckMenuRadioItem(hDelay, IDM_SLOW, IDM_REALTIME, command, MF_BYCOMMAND);
          switch (command) {
            case IDM_SLOW:     g_delay = kSlowSpeed;  break;
            case IDM_MEDIUM:   g_delay = kMedSpeed;   break;
            case IDM_FAST:     g_delay = kHighSpeed;  break;
            case IDM_HYPER:    g_delay = kHyperSpeed; break;
            case IDM_REALTIME: g_delay = kRealTime;   break;
            default:
              LOG(ERROR) << "Unhandled speed type";
              g_delay = g_default_speed;
              break;
          }
          // Replace the timer with the new interval — but only if the
          // simulation is currently running. If we're paused (including
          // mid-Custom-placement before any ants are dropped) the timer
          // is intentionally off, and re-arming it here would silently
          // resume the simulation behind the user's back. The new
          // g_delay is still saved above, so the next resume picks it up.
          if (!g_paused) {
            SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr);
            // Pulse every active thread once so they stop waiting on the
            // old interval — the next WM_TIMER tick fires at the new rate.
            SignalAntsTick();
          }
          break;
        }
        case IDM_TESTTRAP:
          StopPlayWav(); // Stop any playing audio
          // Play the "fahh" sound-effect synchronously so the whole clip
          // gets through before TestTrap fires. SND_ASYNC would let the
          // int3 kill the PlaySound worker mid-playback; SND_SYNC blocks
          // the UI for the clip's duration (~1s) which is fine for a dev
          // menu item that's about to crash the app anyway. SND_NODEFAULT
          // suppresses the system "ding" fallback if the resource is
          // missing.
          PlaySoundW(MAKEINTRESOURCEW(IDR_FAHH_WAVE), g_hInstance,
                     SND_RESOURCE | SND_SYNC | SND_NODEFAULT);
          TestTrap();
          break;
        default:
          return DefWindowProcW(hWnd, message, wParam, lParam);
      }
    } break;
    case WM_LBUTTONDOWN:
      // In Custom place mode the left-click drops an ant at the cursor cell
      // (up to kMaxAntThreads total) instead of starting a window drag.
      if (g_place_mode) {
        PlaceAntAtClient(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        break;
      }
      // Default behavior: left-click drag moves the window.
      ReleaseCapture();
      SendMessageW(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
      break;
    case WM_CONTEXTMENU: {
      // TrackPopupMenu is called with the actual Settings submenu handle from
      // the menu bar. Because it is the same HMENU object, all checkmarks and
      // grayed states are shared automatically — no extra synchronization is
      // needed. WM_COMMAND messages dispatched from the popup go to hWnd and
      // are handled by the existing WM_COMMAND cases below.
      int x = GET_X_LPARAM(lParam);
      int y = GET_Y_LPARAM(lParam);
      // lParam is (-1, -1) when triggered by keyboard (Menu key / Shift+F10).
      // Fall back to the top-left corner of the client area in that case.
      if (x == -1 && y == -1) {
        POINT pt = { 0, 0 };
        ClientToScreen(hWnd, &pt);
        x = pt.x;
        y = pt.y;
      }
      HMENU hSettings = GetSubMenu(GetMenu(hWnd), 1);
      TrackPopupMenu(hSettings, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                     x, y, 0, hWnd, nullptr);
      break;
    }
    case WM_MBUTTONDOWN: {
      RECT rc;
      GetCursorPos(&s_resizeOrigin);
      GetWindowRect(hWnd, &rc);
      s_resizeStartSize = { rc.right - rc.left, rc.bottom - rc.top };
      s_resizing = true;
      SetCapture(hWnd);
      break;
    }
    case WM_MOUSEMOVE: {
      if (s_resizing) {
        POINT pt;
        GetCursorPos(&pt);
        int w = s_resizeStartSize.cx + (pt.x - s_resizeOrigin.x);
        int h = s_resizeStartSize.cy + (pt.y - s_resizeOrigin.y);
        if (w < GetSystemMetrics(SM_CXMINTRACK)) w = GetSystemMetrics(SM_CXMINTRACK);
        if (h < GetSystemMetrics(SM_CYMINTRACK)) h = GetSystemMetrics(SM_CYMINTRACK);
        SetWindowPos(hWnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      break;
    }
    case WM_MBUTTONUP:
      s_resizing = false;
      ReleaseCapture();
      break;
    case WM_CAPTURECHANGED:
      s_resizing = false;
      break;
    case WM_HELP:
      LaunchHelp(hWnd);
      break;
    case WM_CLOSE:
      ShutDownApp();
      break;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_DESTROY:
      // Stop the ants timer first so no more WM_TIMER messages are queued.
      KillTimer(hWnd, TIMER_ANTS);
      // Tear down every ant thread, signal their tick events, and close
      // their handles in one shot.
      ShutdownAnts();
      // Tear down MCI here too — most shutdowns go through ShutDownApp
      // (which calls StopPlayWav), but if the window is destroyed by any
      // other path (external DestroyWindow, session end, etc.) we still
      // need to close the waveform device and delete the temp BGM file.
      // StopPlayWav must come BEFORE ShutdownBgm — it's a sync post to
      // the worker, so the worker has to still be alive to process it.
      // StopPlayWav is a no-op if the device was never opened;
      // ShutdownBgm is a no-op if the worker was never started.
      StopPlayWav();
      ShutdownBgm();
      // Clean up the back buffer. Order matters: DeleteDC first deselects
      // g_hbmMem from the memory DC, after which DeleteObject can safely free
      // the bitmap. Deleting a bitmap that is still selected into a DC is
      // undefined behavior in Win32.
      EnterCriticalSection(&g_paintCS);
      if (g_hdcMem != nullptr) {
        DeleteDC(g_hdcMem);
        g_hdcMem = nullptr;
      }
      if (g_hbmMem != nullptr) {
        DeleteObject(g_hbmMem);
        g_hbmMem = nullptr;
      }
      LeaveCriticalSection(&g_paintCS);
      PostQuitMessage(0);
      break;
    case WM_NCDESTROY:
      mainHwnd = nullptr;
      break;
    default:
      return DefWindowProcW(hWnd, message, wParam, lParam);
  }
  return 0;
}

void ShutDownApp() {
  // Stop the BGM first (sync post to the worker), THEN tear the worker
  // down. Both calls are idempotent — WM_DESTROY will call them again
  // harmlessly on the way out.
  StopPlayWav();
  ShutdownBgm();
  // De-initialize logging, which closes any console window open
  logging::DeInitLogging(g_hInstance); // Can't log anything more after this
  // WM_DESTROY will call ShutdownAnts() for us — DestroyWindow triggers that
  // path synchronously, so we don't need to touch thread state here.
  DestroyWindow(mainHwnd);
}

bool InitApp(HWND hWnd) {
  if (hWnd == nullptr) {
    return false;
  }
  // All settings (number of ants, delay, background color) are already set by
  // InitMenuDefaults.
  if (!ShowAnts()) {
    ErrorBox(hWnd, L"ShowAnts Error", L"ShowAnts failed!");
    return false;
  }
  // Defer the audio kick-off to WM_APP_AUTOPLAY: posting (rather than
  // calling SyncBgm directly here) makes sure the MCI work runs in the
  // normal WindowProc dispatch context once the message loop is pumping.
  // Always post — the handler's SyncBgm is a no-op when g_playsound is
  // false, so it's RC-driven via InitMenuDefaults' IDM_SOUND read with
  // no extra branching here.
  PostMessageW(hWnd, WM_APP_AUTOPLAY, 0, 0);
  return true;
}

bool LaunchHelp(HWND hWnd) {
  if (InfoBox(hWnd, L"Help32", L"No help yet...")) {
    return true;
  }
  return false;
}

INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      // Set icon in titlebar of about dialog
      static const HICON kAboutIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_ABOUT));
      SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)kAboutIcon);
      SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)kAboutIcon);
      return TRUE;
    case WM_CLOSE:
      EndDialog(hDlg, TRUE);
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
      }
      break;
    default:
      break;
  }
  return FALSE;
}
