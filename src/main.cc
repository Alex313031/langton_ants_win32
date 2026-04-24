/*------------------------------------------
   Langton's Ants — Win32 GDI implementation
   Copyright (c) 2026 Alex313031
  ------------------------------------------*/

#include "main.h"

#include "resource.h"
#include "utils.h"
#include "version.h"

HWND mainHwnd = nullptr;

static HINSTANCE g_hInstance = nullptr;

int cxClient = 0;
int cyClient = 0;

static bool s_resizing = false;
static POINT s_resizeOrigin = {};
static SIZE s_resizeStartSize = {};

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
static constexpr bool debug_console = true;

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
  wndclass.hCursor       = LoadCursorW(nullptr, IDC_ARROW) ;
  wndclass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  wndclass.lpszMenuName  = MAKEINTRESOURCEW(IDC_MAIN);
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
  mainHwnd = CreateWindowExW(exStyle, szClassName, appTitle, style,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         CW_WIDTH, CW_HEIGHT, nullptr, nullptr, hInstance, nullptr);

  if (mainHwnd == nullptr) {
    return 1;
  }
  ShowWindow(mainHwnd, iCmdShow);
  if (!UpdateWindow(mainHwnd)) {
    return 1;
  }

  HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDC_MAIN));

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (!TranslateAcceleratorW(mainHwnd, hAccel, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DeleteCriticalSection(&g_paintCS);
  return msg.wParam;
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
    case WM_APP_AUTOPLAY:
      // Deferred startup auto-play. InitApp (called from WM_CREATE) posts
      // this message instead of calling PlayWavFile directly so the actual
      // play call runs in the normal WindowProc dispatch context rather
      // than inside WM_CREATE, which is correct cross-version hygiene.
      // SetSoundButton flips the toolbar button from "Music" (idle) to
      // "Mute" (playing) once playback is underway.
      PlayWavFile(sound_file, kUseEmbeddedBgm);
      SetSoundButton(g_playsound);
      break;
    case MM_MCINOTIFY:
      // Loop-back handler for the MCI-driven background music. When the
      // waveform driver finishes a `play ... notify` command successfully,
      // it posts MM_MCINOTIFY with wParam == MCI_NOTIFY_SUCCESSFUL. We
      // re-issue the play command here to achieve continuous looping —
      // MCIWAVE doesn't accept `repeat` on its play verb the way MCIAVI /
      // MCICDA do, so this is the idiomatic substitute.
      //
      // `from 0` is REQUIRED: after a natural completion MCI leaves the
      // position cursor at end-of-file, and a bare `play` would try to
      // resume from EOF (a silent no-op). Explicitly rewinding to 0
      // restarts the clip cleanly and is correct whether or not the
      // driver auto-rewinds on underflow.
      //
      // wParam values we might see: MCI_NOTIFY_SUCCESSFUL (natural end-of-
      // clip — we loop), MCI_NOTIFY_SUPERSEDED (another play command took
      // over — do nothing), MCI_NOTIFY_ABORTED (stop/close happened — do
      // nothing), MCI_NOTIFY_FAILURE (driver error — do nothing). The
      // g_playsound guard also protects against a late-arriving SUCCESSFUL
      // notification that races a user Stop.
      if (wParam == MCI_NOTIFY_SUCCESSFUL && g_playsound) {
        // Re-issue with hWnd as the callback so the NEXT completion also
        // lands here. Dropping the callback param (passing nullptr) would
        // make MCI silently swallow future MM_MCINOTIFY messages and the
        // loop would play exactly twice then die.
        mciSendStringW(L"play langton_ants_bgm from 0 notify", nullptr, 0, hWnd);
      }
      break;
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
        // Back buffer coords are ant-local (0..cxClient-1, 0..cyClient-1);
        // shifting by the toolbar height places the canvas below the toolbar.
        BitBlt(hdc, 0, g_toolbarHeight, cxClient, cyClient,
               g_hdcMem, 0, 0, SRCCOPY);
      }
      LeaveCriticalSection(&g_paintCS);
      EndPaint(hWnd, &ps);
      break;
    }
    case WM_SIZE: {
      // Let the toolbar re-fit the new parent width and re-measure its height.
      // All toolbar state lives in utils.cc; this one call updates
      // g_toolbarHeight as needed.
      LayoutToolbar(hWnd);
      // cxClient / cyClient represent the ants canvas area, not the parent's client
      // area — the toolbar isn't drawable space. Clamp to zero when the
      // window is smaller than the toolbar (minimized / extreme resize).
      cxClient = LOWORD(lParam);
      cyClient = HIWORD(lParam) - g_toolbarHeight;
      if (cyClient < 0) cyClient = 0;
      // The ants canvas changed size, so recreate the back buffer to match.
      // If it grew, the old bitmap would be too small and BitBlt would read
      // outside its bounds; if it shrank, the old one just wastes memory.
      RecreateBackBuffer(hWnd, cxClient, cyClient);
      break;
    }
    case WM_NOTIFY: {
      // Toolbar dropdown buttons (TBSTYLE_DROPDOWN + TBSTYLE_EX_DRAWDDARROWS)
      // send TBN_DROPDOWN when the user clicks the arrow. We handle it for
      // IDM_ANTS by popping up a small color menu anchored under the button.
      // lParam points to an NMTOOLBAR whose rcButton is in toolbar-client
      // coords — ClientToScreen on the toolbar's HWND (hdr.hwndFrom) gives
      // the screen-coord anchor TrackPopupMenu wants.
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

        if (pnmtb->iItem == IDM_ANTS) {
          // Reuse the Settings → Num Ants submenu directly. Because it's
          // the same HMENU object the main menu bar uses, its radio check
          // mark (which ant count is active) stays in perfect sync with
          // the existing IDM_CONC_* handlers — no duplicate items, no
          // manual state sync needed.
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
          HMENU hAnts   = GetSubMenu(hSettings, 3);
          TrackPopupMenu(hAnts, TPM_LEFTALIGN | TPM_TOPALIGN,
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
            PlaySoundW(L"SystemNotification", nullptr, SND_ASYNC);
          }
          DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_ABOUTDLG), hWnd, AboutDlgProc);
          break;
        case IDM_HELP:
          LaunchHelp(hWnd);
          break;
        case IDM_SAVE_AS:
          SaveClientBitmap(hWnd);
          break;
        case IDM_SOUND: {
          if (ToggleSound()) {
            // Only update check state if toggling sound on/off succeeded.
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
            CheckMenuItem(hSettings, IDM_SOUND,
                          MF_BYCOMMAND | (g_playsound ? MF_CHECKED : MF_UNCHECKED));
            // Mirror the state on the toolbar: swap icon + label.
            SetSoundButton(g_playsound);
          }
          break;
        }
        case IDM_PAUSED: {
          TogglePaintAnts(hWnd);
          // Reflect the new paused state in the menu check mark.
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
          CheckMenuItem(hSettings, IDM_PAUSED,
                        MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
          // Mirror the state on the toolbar: swap icon + label.
          SetPauseButton(g_paused);
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
            HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
            CheckMenuItem(hSettings, IDM_PAUSED, MF_BYCOMMAND | MF_CHECKED);
          }
          // Pulse every active ant thread's tick event once. The timer is
          // off (paused), so this is the only source of ticks — each thread
          // wakes, moves its ant by one space.
          SignalAntsTick();
          break;
        }
        case IDM_REPAINT: {
          // Clear the back buffer to the current background color and force a
          // repaint. All user settings stay intact — the ant thread keeps
          // drawing at the same rate with the same speed/color options,
          // it just has a fresh canvas to paint onto.
          EnterCriticalSection(&g_paintCS);
          if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
            RECT rc = { 0, 0, cxClient, cyClient };
            HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
            FillRect(g_hdcMem, &rc, hBrush);
            DeleteObject(hBrush);
          }
          LeaveCriticalSection(&g_paintCS);
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
        case IDM_CONC_8: {
          // Consecutive IDs let us derive the count directly from the command.
          SetNumAnts((command - IDM_CONC_1) + 1);
          // Num Ants submenu
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
          HMENU hConc     = GetSubMenu(hSettings, 3);
          CheckMenuRadioItem(hConc, IDM_CONC_1, IDM_CONC_8, command, MF_BYCOMMAND);
          break;
        }
        case IDM_MONOCHROME: {
          g_monochrome = !g_monochrome;
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
          // Toggle the check mark on the menu item to show current state.
          CheckMenuItem(hSettings, IDM_MONOCHROME,
                        MF_BYCOMMAND | (g_monochrome ? MF_CHECKED : MF_UNCHECKED));
          // Grey out or restore the color background options — only white and
          // black are valid background choices in monochrome mode.
          HMENU hBkgMenu = GetSubMenu(hSettings, 8);
          const UINT colorState = g_monochrome ? MF_GRAYED : MF_ENABLED;
          EnableMenuItem(hBkgMenu, IDM_RED_BKG,   MF_BYCOMMAND | colorState);
          EnableMenuItem(hBkgMenu, IDM_GREEN_BKG, MF_BYCOMMAND | colorState);
          EnableMenuItem(hBkgMenu, IDM_BLUE_BKG,  MF_BYCOMMAND | colorState);
          // If switching into monochrome and the current background is a color
          // (not white or black), reset it to white.
          if (g_monochrome && g_bkg_color != RGB_WHITE && g_bkg_color != RGB_BLACK) {
            g_bkg_color = RGB_WHITE;
            CheckMenuRadioItem(hBkgMenu, IDM_WHITE_BKG, IDM_BLUE_BKG, IDM_WHITE_BKG, MF_BYCOMMAND);
          }
          // Always clear the back buffer on toggle
          EnterCriticalSection(&g_paintCS);
          if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
            RECT rc = { 0, 0, cxClient, cyClient };
            HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
            FillRect(g_hdcMem, &rc, hBrush);
            DeleteObject(hBrush);
          }
          LeaveCriticalSection(&g_paintCS);
          InvalidateRect(hWnd, nullptr, FALSE);
          break;
        }
        case IDM_WHITE_BKG:
        case IDM_BLACK_BKG:
        case IDM_GREY_BKG:
        case IDM_RED_BKG:
        case IDM_GREEN_BKG:
        case IDM_BLUE_BKG: {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
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
        case IDM_HYPER: {
          HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
          HMENU hDelay    = GetSubMenu(hSettings, 5);
          CheckMenuRadioItem(hDelay, IDM_SLOW, IDM_HYPER, command, MF_BYCOMMAND);
          switch (command) {
            case IDM_SLOW:   g_delay = 1000UL; break;
            case IDM_MEDIUM: g_delay = 500UL; break;
            case IDM_FAST:   g_delay = 250UL; break;
            case IDM_HYPER:  g_delay = 125UL; break;
            default:         g_delay = 250UL; break;
          }
          // Replace the timer with the new interval. SetTimer on an existing ID
          // replaces it in place — the next tick will be at the new rate.
          SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr);
          // Pulse every active thread once so they stop waiting on the old
          // interval — the next WM_TIMER tick will then fire at the new rate.
          SignalAntsTick();
          break;
        }
        case IDM_TESTTRAP:
          TestTrap();
          break;
        default:
          return DefWindowProcW(hWnd, message, wParam, lParam);
      }
    } break;
    case WM_LBUTTONDOWN:
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
      HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
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
  StopPlayWav();
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
  // Only start background music if IDM_SOUND is CHECKED in the RC at startup.
  // Keeps behavior RC-driven: toggling the CHECKED flag in langton_ants.rc is the
  // only change needed to opt in or out of auto-play.
  //
  // We POST a custom message rather than calling PlayWavFile directly. Reason:
  // on Windows 2000, PlaySound's SND_ASYNC worker thread silently discards the
  // decode when invoked from inside WM_CREATE (i.e. before the owning thread's
  // message loop has started pumping). PlaySound returns TRUE but no audio
  // ever comes out. XP and later relaxed this, so the direct call appears to
  // work there — but posting is correct on every version. WM_APP_AUTOPLAY is
  // picked up by the normal dispatch path once GetMessage starts running.
  HMENU hSettings = GetSubMenu(GetMenu(hWnd), 2);
  if (GetMenuState(hSettings, IDM_SOUND, MF_BYCOMMAND) & MF_CHECKED) {
    PostMessageW(hWnd, WM_APP_AUTOPLAY, 0, 0);
  }
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
