#include "ui_utils.h"

#include "ants.h"
#include "globals.h"
#include "resource.h"
#include "sound.h" // g_playsound for HandleToolbarTooltips

// The toolbar child window handle. Kept file-static so nothing else can
// accidentally mutate it - other TUs interact only via the functions below.
static HWND s_hToolbar = nullptr;
// Toolbar's internal tooltip control (TBSTYLE_TOOLTIPS makes the toolbar
// create one for itself). Cached after CreateAppToolbar so HandleToolbarTooltips
// can compare incoming TTN_NEEDTEXT's hwndFrom against it - tooltip
// notifications from OTHER controls (notably the status bar's SBARS_TOOLTIPS
// tooltip) bubble up to the same WM_NOTIFY handler, and without this gate
// they fall through to the default switch arm and log a spurious WARN.
static HWND s_hToolbarTip = nullptr;

// Saved original toolbar WndProc so our subclass can chain through to it.
static WNDPROC s_origToolbarProc = nullptr;

// Measured in pixels after TB_AUTOSIZE runs. Exposed through ui_utils.h so
// ants.cc / main.cc can offset the back-buffer blit and mouse coords by it.
int g_toolbarHeight = 0;

// Measured in pixels after the status bar self-sizes from a WM_SIZE on the
// parent. Subtracted from the canvas height so ant cells don't extend under
// the bar (the bar paints last so visually it'd cover them, but those rows
// would be wasted back-buffer pixels and clicks/place mode would target
// hidden cells).
int g_statusBarHeight = 0;

// True between a UserMessage call and the TIMER_STATUS_RESET firing - i.e.
// the bar is showing a transient user message rather than the default text.
// See ui_utils.h for the full contract.
bool g_status_revert_pending = false;

// Bitmap indices captured from TB_ADDBITMAP for the dynamic-icon buttons.
// TB_ADDBITMAP returns the starting index of images it added to the toolbar's
// internal image list, which is what TBBUTTON::iBitmap (and TBBUTTONINFO::
// iImage) references. SetPauseButton / SetSoundButton toggle between these
// to flip the icon on state changes.
static int s_idxPause  = 0;
static int s_idxPlay   = 0;
static int s_idxStop   = 0;
static int s_idxSound  = 0;
static int s_idxMute   = 0;
static int s_idxAnts   = 0;
static int s_idxSpeed  = 0;
static int s_idxCustom = 0;
static int s_idxCells  = 0;
static int s_idxColors = 0;

// Reads the CHECKED state of every menu group at startup and sets the
// corresponding globals. This makes all defaults entirely RC-driven: changing
// which item has CHECKED in langton_ants.rc is the only code change needed to
// alter a default setting.
void InitMenuDefaults(HWND hWnd) {
  HMENU hMenu = GetMenu(hWnd);
  if (hMenu == nullptr) {
    LOG(ERROR) << L"GetMenu returned null - menu defaults skipped";
    return;
  }
  HMENU hSettings = GetSubMenu(hMenu, 1);
  if (hSettings == nullptr) {
    LOG(ERROR) << L"GetSubMenu(Settings) returned null - menu defaults skipped";
    return;
  }
  HMENU hConc    = GetSubMenu(hSettings, 3); // Num Ants submenu
  HMENU hDelay   = GetSubMenu(hSettings, 4); // Speed menu
  HMENU hBkgMenu = GetSubMenu(hSettings, 5); // Colors menu (bg colors + monochrome)
  if (hConc == nullptr || hDelay == nullptr || hBkgMenu == nullptr) {
    LOG(ERROR) << L"Missing Settings sub-submenu (RC index drift?) "
                  L"hConc="
               << (hConc ? L"set" : L"null") << L" hDelay=" << (hDelay ? L"set" : L"null")
               << L" hBkgMenu=" << (hBkgMenu ? L"set" : L"null");
    return;
  }

  // Background color. Struct named so the for-loop can spell its type
  // explicitly (MSVC 2008 / C++03 has no auto / no decltype).
  struct BkgEntry {
    UINT id;
    COLORREF color;
  };
  static const BkgEntry bkgs[] = {
      {IDM_WHITE_BKG, RGB_WHITE}, {IDM_BLACK_BKG, RGB_BLACK}, {IDM_GREY_BKG, RGB_GREY},
      {IDM_RED_BKG, RGB_RED},     {IDM_GREEN_BKG, RGB_GREEN}, {IDM_BLUE_BKG, RGB_BLUE},
  };
  for (const BkgEntry& bkg : bkgs) {
    if (GetMenuState(hBkgMenu, bkg.id, MF_BYCOMMAND) & MF_CHECKED) {
      g_bkg_color = bkg.color;
      break;
    }
  }

  // Draw delay
  struct DelayEntry {
    UINT id;
    unsigned long ms;
  };
  const DelayEntry delays[] = {
      {IDM_SLOW, kSlowSpeed},   {IDM_MEDIUM, kMedSpeed},   {IDM_FAST, kHighSpeed},
      {IDM_HYPER, kHyperSpeed}, {IDM_REALTIME, kRealTime},
  };
  for (const DelayEntry& delay : delays) {
    if (GetMenuState(hDelay, delay.id, MF_BYCOMMAND) & MF_CHECKED) {
      g_delay         = delay.ms;
      g_default_speed = delay.ms;
      break;
    }
  }

  // Concurrent ants - exactly one IDM_CONC_N must be CHECKED in the RC.
  // IDs are consecutive (IDM_CONC_1..IDM_CONC_16) so we can probe them in a loop.
  for (UINT id = IDM_CONC_1; id <= IDM_CONC_16; ++id) {
    if (GetMenuState(hConc, id, MF_BYCOMMAND) & MF_CHECKED) {
      SetNumAnts((id - IDM_CONC_1) + 1);
      break;
    }
  }

  // Algorithm - exactly one IDM_CLASSIC..IDM_LOGARITHMIC must be CHECKED.
  // Submenu path: Settings (1) -> Customize (7) -> Algorithms (3).
  // Customize layout: CustomPlace, CustomSeed, separator, Algorithms popup.
  HMENU hCustom = GetSubMenu(hSettings, 7);
  HMENU hAlgo   = (hCustom != nullptr) ? GetSubMenu(hCustom, 3) : nullptr;
  if (hAlgo == nullptr) {
    LOG(ERROR) << L"Missing Customize/Algorithms submenu (RC index drift?)";
  } else {
    for (UINT id = IDM_CLASSIC; id <= IDM_LOGARITHMIC; ++id) {
      if (GetMenuState(hAlgo, id, MF_BYCOMMAND) & MF_CHECKED) {
        g_algorithm = static_cast<AntAlgorithm>(id - IDM_CLASSIC);
        break;
      }
    }
  }

  // Show-grid + no-client-bounds toggles. Cell Options is now a top-level
  // entry under Settings (index 9), no longer nested inside Customize.
  HMENU hCells = GetSubMenu(hSettings, 9);
  if (hCells == nullptr) {
    LOG(ERROR) << L"Missing Settings/Cell Options submenu (RC index drift?)";
  } else {
    g_show_grid        = (GetMenuState(hCells, IDM_SHOWGRID, MF_BYCOMMAND) & MF_CHECKED) != 0;
    g_no_client_bounds = (GetMenuState(hCells, IDM_NOCLIENTBOUNDS, MF_BYCOMMAND) & MF_CHECKED) != 0;
  }

  // Sound - seed the user's sound preference from the IDM_SOUND menu
  // check. SyncBgm later (via WM_APP_AUTOPLAY) reads this to decide
  // whether to start playback at startup.
  g_playsound = (GetMenuState(hSettings, IDM_SOUND, MF_BYCOMMAND) & MF_CHECKED) != 0;

  // Ant color - exactly one of the IDM_*ANT items must be CHECKED in
  // the RC. Map the checked one to g_ant_color (kRandomAntColor for
  // the "Random" entry, otherwise the literal RGB).
  struct AntColorEntry {
    UINT id;
    COLORREF color;
  };
  const AntColorEntry antColors[] = {
      {IDM_CYANANT, RGB_CYAN},
      {IDM_YELLOWANT, RGB_YELLOW},
      {IDM_MAGENTAANT, RGB_MAGENTA},
      {IDM_ALLCOLORANT, kRandomAntColor},
  };
  for (const AntColorEntry& antColor : antColors) {
    if (GetMenuState(hSettings, antColor.id, MF_BYCOMMAND) & MF_CHECKED) {
      g_ant_color = antColor.color;
      break;
    }
  }

  // Monochrome toggle - grey out chromatic bg items and the Ant Colors
  // submenu (monochrome forces the marker to the trail color regardless
  // of g_ant_color), and override the RC's bg CHECKED to grey
  // (monochrome defaults to grey bg + white ants regardless of what the
  // RC otherwise selected; white and black remain selectable afterward).
  if (GetMenuState(hSettings, IDM_MONOCHROME, MF_BYCOMMAND) & MF_CHECKED) {
    g_monochrome = true;
    EnableMenuItem(hBkgMenu, IDM_RED_BKG, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_GREEN_BKG, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_BLUE_BKG, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_CYANANT, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_YELLOWANT, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_MAGENTAANT, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_ALLCOLORANT, MF_BYCOMMAND | MF_GRAYED);
    if (g_bkg_color != RGB_GREY) {
      g_bkg_color = RGB_GREY;
      CheckMenuRadioItem(hBkgMenu, IDM_WHITE_BKG, IDM_BLUE_BKG, IDM_GREY_BKG, MF_BYCOMMAND);
    }
  }
}

static void DisableWindowTheme(HWND hWnd) {
  HMODULE hUxTheme = LoadLibraryW(L"uxtheme.dll");
  if (hUxTheme == nullptr) {
    return;
  }
  typedef HRESULT(WINAPI * SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
  SetWindowThemeFn pSetWindowTheme =
      reinterpret_cast<SetWindowThemeFn>(GetProcAddress(hUxTheme, "SetWindowTheme"));
  if (pSetWindowTheme != nullptr) {
    // Empty strings (not nullptr) mean "use no theme" for this window.
    pSetWindowTheme(hWnd, L"", L"");
  }
  FreeLibrary(hUxTheme);
}

// Subclass for the toolbar, handling two things:
//
//   WM_ERASEBKGND - fill the client area with the standard 3D face color.
//     On real Windows this is redundant (the opaque toolbar paints its own
//     background during WM_PAINT anyway), but Wine's toolbar does not
//     reliably fill the background, leaving the control transparent.
//     Painting it here covers Wine without changing anything on real Windows.
//
//   WM_PAINT - chain to the original proc so it draws buttons/background,
//     then draw a single-pixel raised line along the bottom via
//     DrawEdge(BDR_RAISEDOUTER, BF_BOTTOM). This gives the classic
//     early-2000s Win32 separator between toolbar and the content area
//     below. The edge must be drawn AFTER the original paint because the
//     original's button rendering would otherwise overwrite it.
static LRESULT CALLBACK ToolbarSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_ERASEBKGND) {
    HDC hdc = reinterpret_cast<HDC>(wParam);
    RECT rc;
    GetClientRect(hWnd, &rc);
    FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1));
    return TRUE;
  }
  if (msg == WM_PAINT) {
    // Let the toolbar's own paint do its buttons + themed background.
    LRESULT result = CallWindowProcW(s_origToolbarProc, hWnd, msg, wParam, lParam);
    // Now stamp a raised edge around the client rect on top of whatever it
    // drew. GetDC gives a fresh client DC outside the BeginPaint/EndPaint
    // cycle that the original proc used - that's fine, we just need to
    // draw a few lines and release.
    HDC hdc = GetDC(hWnd);
    if (hdc != nullptr) {
      RECT rc;
      GetClientRect(hWnd, &rc);
      // BDR_RAISEDINNER - single-pixel highlight/shadow, subtler than BDR_RAISEDOUTER
      // BF_BOTTOM restricts drawing to one edge.
      DrawEdge(hdc, &rc, BDR_RAISEDINNER | BDR_RAISEDOUTER, BF_BOTTOM);

      // Classic (non-FLAT) toolbars render TBSTYLE_SEP entries as blank
      // gaps rather than visible dividers. Walk the button list ourselves
      // and stamp an etched vertical line into each separator's rect so
      // groups stay visually distinct.
      const int count = static_cast<int>(SendMessageW(hWnd, TB_BUTTONCOUNT, 0, 0));
      for (int i = 0; i < count; i++) {
        TBBUTTON btn = {};
        if (!SendMessageW(hWnd, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(&btn))) {
          continue;
        }
        if (!(btn.fsStyle & TBSTYLE_SEP)) {
          continue;
        }
        RECT ir;
        if (!SendMessageW(hWnd, TB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&ir))) {
          continue;
        }
        // A 2-pixel-wide rect centered in the separator, inset vertically
        // by a couple pixels so the line doesn't touch the toolbar edges.
        const int xMid = (ir.left + ir.right) / 2;
        RECT lineRect  = {xMid - 1, ir.top + 2, xMid + 1, ir.bottom - 2};
        // EDGE_ETCHED + BF_LEFT paints a sunken-outer / raised-inner pair
        // along the left side of the rect, giving a 2-pixel etched line.
        DrawEdge(hdc, &lineRect, EDGE_ETCHED, BF_LEFT);
      }
      ReleaseDC(hWnd, hdc);
    }
    return result;
  }
  return CallWindowProcW(s_origToolbarProc, hWnd, msg, wParam, lParam);
}

// Creates the application's top toolbar as a child of hParent.
//
// A toolbar in Win32 is its own child window of class TOOLBARCLASSNAME
// (provided by the Common Controls DLL). We populate it with buttons that
// pull their images from a "bitmap strip" - a single wide bitmap where each
// button's image is a fixed-size slice. All of this app's toolbar icons are
// loaded from its own resources (IDB_* bitmaps in langton_ants.rc) rather than
// from the comctl32 standard strip, so the look stays consistent across
// Windows versions and theme variations.
//
// Button clicks arrive as WM_COMMAND messages to the parent, with wParam
// low-word set to the button's idCommand. Here we map the save button to
// IDM_SAVE_AS so it shares the existing menu handler - no duplicate code.
bool CreateAppToolbar(HWND hParent, HINSTANCE hInst) {
  bool ok = true;
  // Styles note - we deliberately do NOT use TBSTYLE_FLAT here. Per MSDN it
  // makes the toolbar transparent, meaning the parent is responsible for
  // painting the background. With WS_CLIPCHILDREN on our main window (which
  // we need to keep parent painting out of the toolbar's rect), there is
  // nothing to paint the background, so the area renders as whatever is in
  // the surface - desktop on Win2000, black on XP+ under DWM. Without
  // TBSTYLE_FLAT the toolbar is opaque: it paints its own background, which
  // the theme engine on XP+ handles automatically (themed raised look), and
  // Win2000 falls back to classic 3D raised shading.
  //
  // TBSTYLE_TOOLTIPS - show tooltip popups when the cursor hovers.
  // TBSTYLE_WRAPABLE - when the parent isn't wide enough to fit every
  // button on one row, the toolbar wraps overflow buttons onto a new
  // row instead of clipping them. LayoutToolbar's TB_AUTOSIZE call
  // re-runs the wrap on each WM_SIZE and re-reads the resulting height
  // into g_toolbarHeight, so the ants canvas stays correctly offset
  // even after a wrap.
  // CCS_TOP is the default (toolbar docks to top of parent) so we omit it.
  HWND hTB =
      CreateWindowExW(0, TOOLBARCLASSNAME, nullptr, WS_CHILD | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE,
                      0, 0, CW_USEDEFAULT, CW_USEDEFAULT, hParent, nullptr, hInst, nullptr);
  if (hTB == nullptr) {
    LOG(ERROR) << L"CreateWindowExW for toolbar failed";
    return false;
  }

  // Tell the control which TBBUTTON layout we compiled against so it can
  // adapt if this binary runs against a different Common Controls DLL version.
  SendMessageW(hTB, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

  // Tighten the per-button padding around the icon. Defaults are roughly
  // 7 px horizontal and 6 px vertical per button on most Windows
  // versions, which makes the toolbar visibly taller than the icons need
  // - shrinking the vertical pad is what brings the toolbar height down.
  // LOWORD = horizontal pad, HIWORD = vertical pad.
  // SendMessageW(hTB, TB_SETPADDING, 0, MAKELPARAM(6, 5));

  // --- Bitmap loading ------------------------------------------------------
  // Each TB_ADDBITMAP adds images to the toolbar's internal image list and
  // returns the starting index of the images it just added. That index is
  // what TBBUTTON::iBitmap refers to.
  //
  // All bitmaps come from this app's own resources (hInst = the exe's module
  // handle). Each is a single-image bitmap, so nBitmaps = 1 per call.
  // Pause/Play and Sound/Mute indices are stored in file-statics so
  // Set*Button() can swap between them on state changes.
  TBADDBITMAP tbab = {};
  tbab.hInst       = hInst;
  tbab.nID         = IDB_SAVE_BMP;
  const int idxSave =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_PAUSE_BMP;
  s_idxPause =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_PLAY_BMP;
  s_idxPlay = static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_STOP_BMP;
  s_idxStop = static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_ANTS_BMP;
  s_idxAnts = static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_TIME_BMP;
  s_idxSpeed =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_CUSTOM_BMP;
  s_idxCustom =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_CELLS_BMP;
  s_idxCells =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_COLORS_BMP;
  s_idxColors =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_SOUND_BMP;
  s_idxSound =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_MUTE_BMP;
  s_idxMute = static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID  = IDB_EXIT_BMP;
  const int idxExit =
      static_cast<int>(SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));

  // --- Buttons -------------------------------------------------------------
  // TBBUTTON fields:
  //   iBitmap   - index into the loaded image list (ignored for TBSTYLE_SEP)
  //   idCommand - WM_COMMAND id sent when the button is clicked
  //   fsState   - TBSTATE_ENABLED for clickable (0 for separators)
  //   fsStyle   - TBSTYLE_BUTTON (push button) or TBSTYLE_SEP (gap)
  //   dwData    - app-defined extra data we don't need
  //   iString   - tooltip/label text pointer (cast through INT_PTR)
  // Three separators: one between Save As and Pause (divides file ops
  // from simulation control), one between Stop and Num Ants (divides
  // transport controls from simulation knobs), and one between Sound
  // and Exit (sets Exit apart). All other buttons sit flush.
  TBBUTTON tbButtons[13] = {};

  tbButtons[0].iBitmap   = idxSave;
  tbButtons[0].idCommand = IDM_SAVE_AS;
  tbButtons[0].fsState   = TBSTATE_ENABLED;
  tbButtons[0].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[0].iString   = reinterpret_cast<INT_PTR>(L"Save As");

  tbButtons[1].fsStyle = TBSTYLE_SEP;

  tbButtons[2].iBitmap   = s_idxPause;
  tbButtons[2].idCommand = IDM_PAUSED;
  tbButtons[2].fsState   = TBSTATE_ENABLED;
  tbButtons[2].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[2].iString   = reinterpret_cast<INT_PTR>(L"Pause");

  tbButtons[3].iBitmap   = s_idxStop;
  tbButtons[3].idCommand = IDM_STOP;
  tbButtons[3].fsState   = TBSTATE_ENABLED;
  tbButtons[3].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[3].iString   = reinterpret_cast<INT_PTR>(L"Stop");

  tbButtons[4].fsStyle = TBSTYLE_SEP;

  tbButtons[5].iBitmap   = s_idxAnts;
  tbButtons[5].idCommand = IDM_ANTS;
  tbButtons[5].fsState   = TBSTATE_ENABLED;
  tbButtons[5].fsStyle   = TBSTYLE_BUTTON | TBSTYLE_DROPDOWN;
  tbButtons[5].iString   = reinterpret_cast<INT_PTR>(L"Num Ants");

  tbButtons[6].iBitmap   = s_idxSpeed;
  tbButtons[6].idCommand = IDM_SPEED;
  tbButtons[6].fsState   = TBSTATE_ENABLED;
  tbButtons[6].fsStyle   = TBSTYLE_BUTTON | TBSTYLE_DROPDOWN;
  tbButtons[6].iString   = reinterpret_cast<INT_PTR>(L"Speed");

  tbButtons[7].iBitmap   = s_idxCustom;
  tbButtons[7].idCommand = IDM_CUSTOM;
  tbButtons[7].fsState   = TBSTATE_ENABLED;
  tbButtons[7].fsStyle   = TBSTYLE_BUTTON | TBSTYLE_DROPDOWN;
  tbButtons[7].iString   = reinterpret_cast<INT_PTR>(L"Customize");

  tbButtons[8].iBitmap   = s_idxCells;
  tbButtons[8].idCommand = IDM_CELLOPTIONS;
  tbButtons[8].fsState   = TBSTATE_ENABLED;
  tbButtons[8].fsStyle   = TBSTYLE_BUTTON | TBSTYLE_DROPDOWN;
  tbButtons[8].iString   = reinterpret_cast<INT_PTR>(L"Cell Options");

  tbButtons[9].iBitmap   = s_idxColors;
  tbButtons[9].idCommand = IDM_COLORS;
  tbButtons[9].fsState   = TBSTATE_ENABLED;
  tbButtons[9].fsStyle   = TBSTYLE_BUTTON | TBSTYLE_DROPDOWN;
  tbButtons[9].iString   = reinterpret_cast<INT_PTR>(L"Colors");

  tbButtons[10].iBitmap   = s_idxSound;
  tbButtons[10].idCommand = IDM_SOUND;
  tbButtons[10].fsState   = TBSTATE_ENABLED;
  tbButtons[10].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[10].iString   = reinterpret_cast<INT_PTR>(L"Sound");

  tbButtons[11].fsStyle = TBSTYLE_SEP;

  tbButtons[12].iBitmap   = idxExit;
  tbButtons[12].idCommand = IDM_EXIT;
  tbButtons[12].fsState   = TBSTATE_ENABLED;
  tbButtons[12].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[12].iString   = reinterpret_cast<INT_PTR>(L"Exit");

  SendMessageW(hTB, TB_ADDBUTTONS, sizeof(tbButtons) / sizeof(tbButtons[0]),
               reinterpret_cast<LPARAM>(tbButtons));

  // Enable split-button dropdown arrows. Without this, TBSTYLE_DROPDOWN makes
  // the entire button act as a dropdown and the button body stops sending a
  // normal WM_COMMAND. With TBSTYLE_EX_DRAWDDARROWS, the arrow is rendered as
  // a separate clickable region: clicking the button body still sends
  // WM_COMMAND (idCommand = IDM_ANTS), while clicking the arrow sends
  // TBN_DROPDOWN via WM_NOTIFY so the parent can pop up a custom menu.
  SendMessageW(hTB, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);

  // Install the subclass for Wine compatibility (see ToolbarSubclassProc).
  // Real Windows ignores it because its WM_PAINT paints over what our
  // WM_ERASEBKGND filled, but Wine needs it to avoid a transparent bar.
  s_origToolbarProc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(hTB, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ToolbarSubclassProc)));

  // Turn off Visual Styles for this toolbar so every button gets the
  // classic always-visible raised bevel, not just on hover. No-op on Win2K
  // (no uxtheme.dll).
  DisableWindowTheme(hTB);

  // TB_AUTOSIZE tells the toolbar to re-measure itself based on its buttons
  // and the parent's width. Required after adding/removing buttons and also
  // on every parent resize (LayoutToolbar calls it again from WM_SIZE).
  SendMessageW(hTB, TB_AUTOSIZE, 0, 0);

  // Buttons and layout are in place; show the toolbar now.
  if (ShowWindow(hTB, SW_SHOW)) {
    // If the window was previously visible, the return value is nonzero (TRUE).
    LOG(DEBUG) << L"Toolbar was already visible!";
  }

  // Store the handle and measure the initial height. GetWindowRect returns
  // screen coords, but for a toolbar docked at the top the height component
  // is what we need regardless.
  s_hToolbar = hTB;
  // Cache the toolbar's tooltip HWND so HandleToolbarTooltips can filter
  // out tooltip notifications coming from other controls (e.g. the status
  // bar's SBARS_TOOLTIPS tooltip).
  s_hToolbarTip = reinterpret_cast<HWND>(SendMessageW(hTB, TB_GETTOOLTIPS, 0, 0));
  RECT tbRect;
  GetWindowRect(hTB, &tbRect);
  g_toolbarHeight = tbRect.bottom - tbRect.top;
  return ok;
}

void LayoutToolbar(HWND hWnd) {
  if (s_hToolbar == nullptr || hWnd == nullptr) {
    return;
  }
  // Let the toolbar re-measure itself for the new parent width. We then re-read
  // its height in case the row count changed (e.g. buttons wrapped onto a new
  // row because the parent got narrower).
  SendMessageW(s_hToolbar, TB_AUTOSIZE, 0, 0);
  RECT tbRect;
  GetWindowRect(s_hToolbar, &tbRect);
  g_toolbarHeight = tbRect.bottom - tbRect.top;
}

// TB_SETBUTTONINFO updates any subset of a TBBUTTON's fields by command ID.
// dwMask picks which fields to apply; here we want the icon and the text.
// The toolbar copies the text string internally, so passing a string literal
// via const_cast is safe - the control won't mutate the memory we point at.
void SetPauseButton(bool paused) {
  if (s_hToolbar == nullptr) {
    LOG(ERROR) << L"SetPauseButton() called before toolbar is ready";
    return;
  }
  TBBUTTONINFOW bi = {};
  bi.cbSize        = sizeof(bi);
  bi.dwMask        = TBIF_IMAGE | TBIF_TEXT;
  bi.iImage        = paused ? s_idxPlay : s_idxPause;
  // Three label states sharing one button:
  //   not paused            "Pause"  (running, click to pause)
  //   paused, mid-run       "Resume" (was playing, click to continue)
  //   paused, fresh/stopped "Play"   (no animation yet OR after IDM_STOP)
  // Both paused variants use the play icon since both start the timer.
  const wchar_t* label;
  if (!paused) {
    label = L"Pause";
  } else if (g_stopped) {
    label = L"Play";
  } else {
    label = L"Resume";
  }
  bi.pszText = const_cast<LPWSTR>(label);
  SendMessageW(s_hToolbar, TB_SETBUTTONINFOW, IDM_PAUSED, reinterpret_cast<LPARAM>(&bi));
}

void SetSoundButton(bool playing) {
  if (s_hToolbar == nullptr) {
    LOG(ERROR) << L"SetSoundButton() called before toolbar is ready";
    return;
  }
  TBBUTTONINFOW bi = {};
  bi.cbSize        = sizeof(bi);
  bi.dwMask        = TBIF_IMAGE | TBIF_TEXT;
  bi.iImage        = playing ? s_idxMute : s_idxSound;
  bi.pszText       = const_cast<LPWSTR>(playing ? L"Mute" : L"Sound");
  SendMessageW(s_hToolbar, TB_SETBUTTONINFOW, IDM_SOUND, reinterpret_cast<LPARAM>(&bi));
}

void SetNumAntsCheck(unsigned int num) {
  if (mainHwnd == nullptr) {
    return;
  }
  HMENU hMenu = GetMenu(mainHwnd);
  if (hMenu == nullptr) {
    return;
  }
  HMENU hSettings = GetSubMenu(hMenu, 1);
  if (hSettings == nullptr) {
    return;
  }
  HMENU hConc = GetSubMenu(hSettings, 3);
  if (hConc == nullptr) {
    return;
  }
  if (num >= 1 && num <= 16) {
    CheckMenuRadioItem(hConc, IDM_CONC_1, IDM_CONC_16, IDM_CONC_1 + (num - 1), MF_BYCOMMAND);
    CheckMenuItem(hConc, IDM_CONC_CUSTOM, MF_BYCOMMAND | MF_UNCHECKED);
  } else {
    // Out of menu range - clear every numeric radio and let the Custom Num
    // entry carry the check mark on its own.
    for (UINT id = IDM_CONC_1; id <= IDM_CONC_16; ++id) {
      CheckMenuItem(hConc, id, MF_BYCOMMAND | MF_UNCHECKED);
    }
    CheckMenuItem(hConc, IDM_CONC_CUSTOM, MF_BYCOMMAND | MF_CHECKED);
  }
}

void SetAlgorithmCheck(AntAlgorithm algo) {
  if (mainHwnd == nullptr) {
    return;
  }
  HMENU hMenu = GetMenu(mainHwnd);
  if (hMenu == nullptr) {
    return;
  }
  HMENU hSettings = GetSubMenu(hMenu, 1);
  if (hSettings == nullptr) {
    return;
  }
  HMENU hCustom = GetSubMenu(hSettings, 7);
  if (hCustom == nullptr) {
    return;
  }
  // Algorithms is the 4th item in Customize (index 3): CustomPlace,
  // CustomSeed, separator, then this popup. Cell Options moved out to
  // a top-level Settings entry.
  HMENU hAlgo = GetSubMenu(hCustom, 3);
  if (hAlgo == nullptr) {
    return;
  }
  // IDM_CLASSIC..IDM_LOGARITHMIC are consecutive and in the same order as
  // AntAlgorithm's underlying values, so the radio target is just the offset.
  const UINT id = IDM_CLASSIC + static_cast<UINT>(algo);
  CheckMenuRadioItem(hAlgo, IDM_CLASSIC, IDM_LOGARITHMIC, id, MF_BYCOMMAND);
}

void SetCustomSeedCheck(bool active) {
  if (mainHwnd == nullptr) {
    return;
  }
  HMENU hMenu = GetMenu(mainHwnd);
  if (hMenu == nullptr) {
    return;
  }
  HMENU hSettings = GetSubMenu(hMenu, 1);
  if (hSettings == nullptr) {
    return;
  }
  HMENU hCustom = GetSubMenu(hSettings, 7);
  if (hCustom == nullptr) {
    return;
  }
  CheckMenuItem(hCustom, IDM_CUSTOMSEED, MF_BYCOMMAND | (active ? MF_CHECKED : MF_UNCHECKED));
}

bool PopupUnderToolbarButton(HWND hOwner, int idCommand, HMENU hMenu) {
  bool ok = true;
  if (s_hToolbar == nullptr || hMenu == nullptr) {
    LOG(ERROR) << L"PopupUnderToolbarButton: toolbar=" << (s_hToolbar ? L"set" : L"null")
               << L", hMenu=" << (hMenu ? L"set" : L"null")
               << L" (called before toolbar ready or with null menu)";
    return false;
  }
  // TB_GETRECT returns the button's rect in toolbar-client coords.
  RECT rc;
  if (!SendMessageW(s_hToolbar, TB_GETRECT, idCommand, reinterpret_cast<LPARAM>(&rc))) {
    LOG(ERROR) << L"PopupUnderToolbarButton: TB_GETRECT failed for "
                  L"command id "
               << idCommand << L" (button missing from the toolbar?)";
    return false;
  }
  // Convert the bottom-left corner to screen space - that's where
  // TrackPopupMenu wants its anchor.
  POINT pt = {rc.left, rc.bottom};
  ClientToScreen(s_hToolbar, &pt);
  if (!TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hOwner, nullptr)) {
    LOG(ERROR) << L"PopupUnderToolbarButton: TrackPopupMenu failed for "
                  L"command id "
               << idCommand;
    ok = false;
  }
  return ok;
}

bool HandleToolbarTooltips(NMHDR* pnmh) {
  if (pnmh == nullptr || s_hToolbar == nullptr) {
    return false;
  }
  // TTN_GETDISPINFOW and TTN_NEEDTEXTW have the same numeric value; accepting
  // both keeps us portable across comctl32 versions. For ANSI comctl the code
  // would be TTN_NEEDTEXTA, but this app is Unicode-only so we ignore that.
  if (pnmh->code != TTN_GETDISPINFOW && pnmh->code != TTN_NEEDTEXTW) {
    return false;
  }
  // Filter by tooltip control: status bar's SBARS_TOOLTIPS notifications also
  // bubble up here with idFrom = part index (0 / 1), which would land in our
  // default switch arm and log "no tooltip for command id 0". s_hToolbarTip
  // is null only if TB_GETTOOLTIPS failed at toolbar creation - in that
  // unlikely case we fall through and the warn would be on us anyway.
  if (s_hToolbarTip != nullptr && pnmh->hwndFrom != s_hToolbarTip) {
    return false;
  }
  NMTTDISPINFOW* pdi  = reinterpret_cast<NMTTDISPINFOW*>(pnmh);
  const int idCommand = static_cast<int>(pdi->hdr.idFrom);

  // Descriptive tooltip strings per button. State-toggling buttons read the
  // corresponding global (g_paused / g_playsound) to pick the right
  // variant. Strings here are owned by the process (string literals or
  // statics), so assigning their pointers to lpszText is safe.
  const wchar_t* text = nullptr;
  switch (idCommand) {
    case IDM_SAVE_AS:
      text = L"Save current ant field as .bmp";
      break;
    case IDM_EXIT:
      text = L"Exit App";
      break;
    case IDM_ANTS:
      text = L"Choose how many ants to spawn";
      break;
    case IDM_SPEED:
      text = L"Change iteration (crawling) speed";
      break;
    case IDM_CUSTOM:
      text = L"Customize ant placement, algorithm, and starting \"seed\"";
      break;
    case IDM_CELLOPTIONS:
      text = L"Cell options: grid lines, canvas bounds, cell size";
      break;
    case IDM_COLORS:
      text = L"Choose color options";
      break;
    case IDM_PAUSED:
      // Mirror the three-state label SetPauseButton picks: "Play Ants"
      // (paused + stopped), "Resume Ants" (paused mid-run), or "Pause Ants".
      if (!g_paused) {
        text = L"Pause Ants";
      } else if (g_stopped) {
        text = L"Play Ants";
      } else {
        text = L"Resume Ants";
      }
      break;
    case IDM_STOP:
      text = L"Stop ants and clear the ant field";
      break;
    case IDM_SOUND:
      text = g_playsound ? L"Mute Background Sounds" : L"Play Background Sounds";
      break;
    default:
      LOG(WARN) << L"HandleToolbarTooltips(): no tooltip for command id " << idCommand;
      return false; // unknown button - let the default handling run
  }
  pdi->lpszText = const_cast<LPWSTR>(text);
  return true;
}

void UpdateStatusBar(const unsigned int part, const std::wstring& text) {
  // DCHECK trips in dev builds if a caller passes an out-of-range part index.
  // Goes BEFORE the early-return - DCHECKs only fire when the condition is
  // false, so checking after the if (part > 1) return; would never trip.
  DCHECK(part <= 1);
  if (part > 1) {
    return;
  }
  if (hStatusBar == nullptr) {
    LOG(ERROR) << L"hStatusBar was null when trying to update text!";
    return;
  }
  const std::wstring out = L" " + text;
  SendMessageW(hStatusBar, SB_SETTEXT, static_cast<WPARAM>(part), (LPARAM)out.c_str());
}

void UserMessage(const std::wstring& message) {
  LOG(INFO) << message;
  if (hStatusBar != nullptr) {
    UpdateStatusBar(0, message);
    // Re-arm the auto-revert timer. SetTimer with the same ID REPLACES the
    // existing timer (per MSDN), restarting the countdown - so a flurry of
    // UserMessages keeps the most recent visible for the full delay rather
    // than letting an old timer revert it early. WM_TIMER for
    // TIMER_STATUS_RESET kills the timer (one-shot) and restores
    // kDefaultStatusText to part 0.
    if (mainHwnd != nullptr) {
      SetTimer(mainHwnd, TIMER_STATUS_RESET, static_cast<UINT>(kStatusBarResetDelay), nullptr);
      // Mark that the bar is showing a transient message so the
      // minimize/restore path knows to suspend + re-arm the revert.
      g_status_revert_pending = true;
    }
  }
}
