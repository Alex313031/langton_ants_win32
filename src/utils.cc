#include "utils.h"

#include <shlwapi.h>

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

// Measured in pixels after TB_AUTOSIZE runs. Exposed through globals.h so
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
// See utils.h for the full contract.
bool g_status_revert_pending = false;

unsigned long g_default_speed = kHighSpeed;

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

const std::wstring GetExeDir() {
  wchar_t exe_path[MAX_PATH];
  HMODULE this_app = GetModuleHandleW(nullptr);
  if (!this_app) {
    return std::wstring();
  }
  DWORD got_path = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (got_path == 0 || got_path >= MAX_PATH) {
    return std::wstring();
  }

  // Find the last backslash to get the directory
  std::wstring fullPath(exe_path);
  size_t lastSlash = fullPath.find_last_of(L"\\/");
  std::wstring retval;
  if (lastSlash != std::wstring::npos) {
    retval = fullPath.substr(0, lastSlash + 1); // Include trailing slash
  } else {
    retval = fullPath;
  }
  return retval;
}

// Opens a system Save As dialog and writes the current back buffer to a 32-bit
// BMP file at the path the user chose.
//
// BMP layout (no palette for 32-bit):
//   BITMAPFILEHEADER  (14 bytes) - magic 'BM', file size, pixel data offset
//   BITMAPINFOHEADER  (40 bytes) - dimensions, bit depth, compression
//   Pixel data        (w * h * 4 bytes) - 32-bit BGRA, bottom-up row order
bool SaveClientBitmap(HWND hWnd) {
  // Prompt the user for a destination path
  wchar_t szFile[MAX_PATH] = {};
  OPENFILENAMEW ofn        = {};
  ofn.lStructSize          = sizeof(OPENFILENAMEW);
  ofn.hwndOwner            = hWnd;
  ofn.lpstrFile            = szFile;
  ofn.nMaxFile             = MAX_PATH;
  ofn.lpstrFilter          = L"Bitmap Files (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
  ofn.nFilterIndex         = 1;
  ofn.lpstrDefExt          = L"bmp";
  ofn.lpstrTitle           = L"Save Bitmap As";
  ofn.Flags                = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

  if (!GetSaveFileNameW(&ofn)) {
    // CommDlgExtendedError() returns 0 when the user cancelled and a
    // non-zero CDERR_* code on actual dialog failure.
    const DWORD err = CommDlgExtendedError();
    if (err != 0) {
      LOG(WARN) << L"GetSaveFileNameW failed (CommDlgExtendedError=" << logging::Hex(err) << L")";
    } else {
      LOG(INFO) << L"Save Bitmap dialog cancelled by user";
    }
    return false;
  }

  // Hold the back buffer lock for the duration of the pixel read so the ant
  // thread cannot modify the bitmap mid-capture.
  EnterCriticalSection(&g_paintCS);

  if (g_hdcMem == nullptr || g_hbmMem == nullptr) {
    LeaveCriticalSection(&g_paintCS);
    return false;
  }

  // Query the actual bitmap dimensions from the GDI object
  BITMAP bm = {};
  GetObjectW(g_hbmMem, sizeof(BITMAP), &bm);
  const int width  = bm.bmWidth;
  const int height = bm.bmHeight;

  if (width <= 0 || height <= 0) {
    LeaveCriticalSection(&g_paintCS);
    return false;
  }

  // Describe the desired output: 32-bit bottom-up RGB (the standard BMP layout)
  // biHeight positive = bottom-up, which is what all BMP readers expect.
  BITMAPINFOHEADER bi = {};
  bi.biSize           = sizeof(BITMAPINFOHEADER);
  bi.biWidth          = width;
  bi.biHeight         = height;
  bi.biPlanes         = 1;
  bi.biBitCount       = 32;
  bi.biCompression    = BI_RGB;
  bi.biSizeImage      = static_cast<DWORD>(width * height * 4);

  // GetDIBits copies the selected bitmap's pixels into our buffer in the format
  // described by bi. With BI_RGB and 32 bits, each pixel is 4 bytes: BGRA
  // (GDI leaves the alpha byte as 0, which is fine for BMP).
  std::vector<BYTE> pixels(bi.biSizeImage);
  GetDIBits(g_hdcMem, g_hbmMem, 0, static_cast<UINT>(height), pixels.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

  LeaveCriticalSection(&g_paintCS);

  // Build the BMP file header
  const DWORD pixelDataOffset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  BITMAPFILEHEADER bf         = {};
  bf.bfType                   = 0x4D42; // 'BM' signature
  bf.bfSize                   = pixelDataOffset + bi.biSizeImage;
  bf.bfOffBits                = pixelDataOffset;

  // Write the three sections to the file
  HANDLE hFile =
      CreateFileW(szFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << L"Failed to open file for writing: " << szFile;
    return false;
  }

  DWORD written;
  const bool ok = WriteFile(hFile, &bf, sizeof(bf), &written, nullptr) &&
                  WriteFile(hFile, &bi, sizeof(bi), &written, nullptr) &&
                  WriteFile(hFile, pixels.data(), bi.biSizeImage, &written, nullptr);

  CloseHandle(hFile);
  if (ok) {
    UserMessage(std::wstring(L"Saved canvas to ") + szFile);
  } else {
    LOG(WARN) << L"Failed to write canvas BMP to " << szFile;
  }
  return ok;
}

void TestTrap(const bool dcheck) {
  static constexpr bool test_trap = true;
  if (dcheck) {
    DCHECK(!test_trap);
  } else {
    CHECK(!test_trap);
  }
  return;
}

bool FillRectWithColor(HDC hdc, const RECT& rc, COLORREF color) {
  bool ok = true;
  if (hdc == nullptr) {
    LOG(ERROR) << L"FillRectWithColor() HDC was null!";
    return false;
  }
  HBRUSH hBrush = CreateSolidBrush(color);
  if (hBrush == nullptr) {
    LOG(ERROR) << L"CreateSolidBrush failed";
    return false;
  }
  if (!FillRect(hdc, &rc, hBrush)) {
    LOG(ERROR) << L"FillRect returned 0!";
    ok = false;
  }
  DeleteObject(hBrush);
  return ok;
}

// MessageBoxW with MB_OK can be dismissed several ways the user considers
// equivalent: clicking OK (IDOK), clicking the X close button (IDCANCEL),
// or pressing Esc (IDCANCEL). All of those mean "the box showed and the
// user dismissed it" - which is what these helpers want to report as
// success. Only a 0 return means the box failed to display in the first
// place (bad hWnd, OOM, no desktop access, etc.); that's the real false.
// `hWnd ? hWnd : mainHwnd` falls back to the main window when the caller
// passed null - useful from helpers that don't have an hWnd of their own.
bool InfoBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  return MessageBoxW(hWnd ? hWnd : mainHwnd, message.c_str(), title.c_str(),
                     MB_OK | MB_ICONINFORMATION) != 0;
}

bool WarnBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  return MessageBoxW(hWnd ? hWnd : mainHwnd, message.c_str(), title.c_str(),
                     MB_OK | MB_ICONWARNING) != 0;
}

bool ErrorBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  return MessageBoxW(hWnd ? hWnd : mainHwnd, message.c_str(), title.c_str(),
                     MB_OK | MB_ICONERROR) != 0;
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
// All toolbar state and logic live here. main.cc only calls CreateAppToolbar
// (from WM_CREATE) and LayoutToolbar (from WM_SIZE); the handle itself never
// escapes this file.

// Disables Visual Styles (theming) on a single window by dynamically loading
// uxtheme.dll and calling SetWindowTheme with empty theme/class strings.
// XP+ themed toolbars render buttons as flat panels that only show a raised
// outline on hover; disabling the theme falls back to the classic renderer
// which gives every button a permanent 3D raised bevel - the look consistent
// with the Win2000/XP-Classic appearance across every Windows version.
//
// We bind dynamically rather than linking uxtheme.lib so the binary still
// loads on Windows 2000 (where uxtheme.dll does not exist). If LoadLibrary
// fails there is nothing to disable anyway - classic rendering is already
// in effect - so we just return quietly.
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
  ShowWindow(hTB, SW_SHOW);

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
  //   not paused           -> "Pause"   (running, click to pause)
  //   paused, mid-run      -> "Resume"  (was playing, click to continue)
  //   paused, fresh/stopped -> "Play"   (no animation yet OR after IDM_STOP)
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

bool ValidateCustomSeed(LPCWSTR cSeed) {
  bool is_valid = false;
  // nullptr check first so the *cSeed dereference below is safe.
  if (cSeed == nullptr || *cSeed == L'\0') {
    return false; // Early fail on null pointer or empty string.
  }
  // Check that all characters are digits
  for (const wchar_t* p = cSeed; *p != L'\0'; ++p) {
    if (*p < L'0' || *p > L'9') {
      return false;
    }
  }
  // Convert to integer. wcstol is the standard wide-string variant
  // (wcstoi is non-standard and missing under MinGW/Clang). long is wider
  // than int on every Windows toolchain we target, so the INT_MAX bound
  // check below still catches the same overflow cases as before.
  wchar_t* end;
  const long seedValue = wcstol(cSeed, &end, 10);
  // Check conversion was successful (end should point to null terminator)
  if (*end != L'\0') {
    return false;
  }
  if (seedValue == 0) {
    LOG(WARN) << L"Custom seed value was 0!";
    is_valid = false;
  } else {
    is_valid = (seedValue > 0 && seedValue <= INT_MAX);
  }
  return is_valid;
}

bool ValidateCustomNum(LPCWSTR cNum) {
  bool is_valid = false;
  // nullptr check first so the *cNum dereference below is safe.
  if (cNum == nullptr || *cNum == L'\0') {
    return false; // Early fail on null pointer or empty string.
  }
  // Check that all characters are digits
  for (const wchar_t* p = cNum; *p != L'\0'; ++p) {
    if (*p < L'0' || *p > L'9') {
      return false;
    }
  }
  // Convert to integer. wcstol is the standard wide-string variant
  // (wcstoi is non-standard and missing under MinGW/Clang).
  wchar_t* end;
  const int antNumValue = wcstol(cNum, &end, 10);
  // Check conversion was successful (end should point to null terminator)
  if (*end != L'\0') {
    return false;
  }
  if (antNumValue == 0) {
    LOG(WARN) << L"Custom ants value was 0!";
    is_valid = false;
  } else if (antNumValue > kMaxAntThreads) {
    LOG(WARN) << L"Custom ants value higher than " << kMaxAntThreads << L"!";
    is_valid = false;
  } else {
    // Check that custom num is less than hard limit
    is_valid = (antNumValue > 0 && antNumValue <= kMaxAntThreads);
  }
  return is_valid;
}

bool ValidateCellSize(LPCWSTR cCell) {
  bool is_valid = false;
  // nullptr check first so the *cCell dereference below is safe.
  if (cCell == nullptr || *cCell == L'\0') {
    return false; // Early fail on null pointer or empty string.
  }
  // Check that all characters are digits
  for (const wchar_t* p = cCell; *p != L'\0'; ++p) {
    if (*p < L'0' || *p > L'9') {
      return false;
    }
  }
  wchar_t* end;
  const long cellsValue = wcstol(cCell, &end, 10);
  // Check conversion was successful (end should point to null terminator)
  if (*end != L'\0') {
    return false;
  }
  if (cellsValue == 0) {
    LOG(WARN) << L"Custom cell size value was 0!";
    is_valid = false;
  } else {
    is_valid = (cellsValue >= MIN_CELL_PX && cellsValue <= MAX_CELL_PX);
  }
  return is_valid;
}

// Updates text in a specified part of the status bar.
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

const std::wstring GetVersionString() {
  // VERSION_STRING is a narrow C string literal built by stringize macros,
  // so we can't feed it straight to std::wstring. Build the wide form
  // directly from the same integer macros (single source of truth in
  // version.h) - std::to_wstring keeps it standards-clean across MinGW
  // and MSVC alike.
  return std::to_wstring(MAJOR_VERSION) + L"." + std::to_wstring(MINOR_VERSION) + L"." +
         std::to_wstring(BUILD_VERSION);
}

const std::wstring GetAppName() {
  const std::wstring app_name = std::wstring(APP_NAME);
  return app_name;
}

bool IsWindowsXpOrLater() {
  OSVERSIONINFOW osvi      = {};
  osvi.dwOSVersionInfoSize = sizeof(osvi);
  if (!GetVersionExW(&osvi)) {
    return false;
  }
  // Major 6+ covers Vista / 7 / 8 / 10 / 11. Major 5 splits: 5.0 = Win2000
  // (the case we want to exclude), 5.1 = XP, 5.2 = Server 2003 / XP x64.
  static const bool isVistaOrNewer   = osvi.dwMajorVersion >= 6;
  static const bool isXpOrServer2003 = osvi.dwMajorVersion == 5 && osvi.dwMinorVersion >= 1;
  return isVistaOrNewer || isXpOrServer2003;
}

static DWORD GetCommCtrlVersion() {
  static const wchar_t* kComCtl32Dll = L"comctl32.dll";
  // Resolve the system comctl32.dll path explicitly. GetSystemDirectoryW
  // returns 0 on failure, or >= MAX_PATH if our buffer was too small (in
  // which case it reports the required size). Either is fatal for us -
  // bail rather than fall through with an empty path that would let
  // LoadLibraryW search the standard DLL order and silently bypass the
  // "explicitly use the system one" intent.
  wchar_t systemDir[MAX_PATH];
  const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    const DWORD error = GetLastError();
    LOG(ERROR) << L"Failed to get system directory! Error: " << logging::Hex(error);
    return 0;
  }
  const std::wstring comctl32_path = std::wstring(systemDir) + L"\\" + kComCtl32Dll;

  HINSTANCE hComCtl32Dll = LoadLibraryW(comctl32_path.c_str());
  if (hComCtl32Dll == nullptr) {
    const DWORD error = GetLastError();
    LOG(ERROR) << L"Failed to load " << kComCtl32Dll << L", hComCtl32Dll was null! Error: "
               << logging::Hex(error);
    return 0;
  }

  DWORD dwVersion = 0;
  DLLGETVERSIONPROC pDllGetVersion =
      reinterpret_cast<DLLGETVERSIONPROC>(GetProcAddress(hComCtl32Dll, "DllGetVersion"));
  if (pDllGetVersion == nullptr) {
    const DWORD error = GetLastError();
    LOG(ERROR) << L"Failed to get DllGetVersion address. Error: " << logging::Hex(error);
  } else {
    DLLVERSIONINFO dvi = {sizeof(dvi)};
    const HRESULT hr   = pDllGetVersion(&dvi);
    if (hr == S_OK) {
      dwVersion = _PACKVERSION(dvi.dwMajorVersion, dvi.dwMinorVersion);
    } else {
      LOG(ERROR) << L"Failed to run DllGetVersion. HRESULT: " << logging::Hex(hr);
    }
  }
  FreeLibrary(hComCtl32Dll);
  return dwVersion;
}

bool IsCommCtrlAtLeast(const DWORD to_compare) {
  const DWORD kCommCtrlVer = GetCommCtrlVersion();
  LOG(DEBUG) << L"Target common controls version: " << logging::Hex(to_compare);
  LOG(DEBUG) << L"Installed common controls version: " << logging::Hex(kCommCtrlVer);
  return kCommCtrlVer >= to_compare;
}

bool IsRunningOnWine(std::string* outWineVer) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return false;
  }
  // Cleaner one-liner via a typedef than splitting the function-pointer
  // declaration and the assignment across two lines.
  typedef const char*(CDECL * WineGetVersion_t)(void);
  const WineGetVersion_t pwine_get_version =
      reinterpret_cast<WineGetVersion_t>(GetProcAddress(ntdll, "wine_get_version"));
  if (pwine_get_version == nullptr) {
    return false;
  }
  // Wine's implementation always returns a valid string in practice, but
  // std::string(nullptr) is undefined behavior - guard it.
  const char* wineVer = pwine_get_version();
  if (wineVer == nullptr) {
    return false;
  }
  // outWineVer is optional: callers that only care about the bool can pass
  // nullptr. Without this null-check we'd crash on the dereference below.
  if (outWineVer != nullptr) {
    *outWineVer = wineVer;
  }
  return true;
}

bool GetRawNtVersion(UINT* major, UINT* minor, UINT* build) {
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  if (hNtDll == nullptr) {
    return false;
  }
  const RtlGetNtVersionNumbers_t pfnRtlGetNtVersionNumbers =
      reinterpret_cast<RtlGetNtVersionNumbers_t>(GetProcAddress(hNtDll, "RtlGetNtVersionNumbers"));
  if (pfnRtlGetNtVersionNumbers == nullptr) {
    LOG(DEBUG) << L"RtlGetNtVersionNumbers not found. (Win 2K?)";
    return false;
  }
  DWORD majorVer = 0;
  DWORD minorVer = 0;
  DWORD buildVer = 0;
  pfnRtlGetNtVersionNumbers(&majorVer, &minorVer, &buildVer);
  if (majorVer == 0) {
    return false; // Should never be zero
  }
  if (is_dcheck) {
    LOG(DEBUG) << L"Raw NT MajorVersion: " << logging::Hex(majorVer);
    LOG(DEBUG) << L"Raw NT MinorVersion: " << logging::Hex(minorVer);
    LOG(DEBUG) << L"Raw NT BuildNumber: " << logging::Hex(buildVer);
  }
  // RtlGetNtVersionNumbers packs the build-type flag into the top 4 bits
  // of the build number: 0xC0000000 = checked (debug) build, 0xF0000000 =
  // free (release) build. Mask them off so callers see the same plain
  // build number the OS reports everywhere else (e.g. 2600 on XP SP3,
  // 7601 on Win7 SP1, 19045 on a recent Win10) instead of the raw
  // 0xF0000A28 = 4026534440 mess.
  const DWORD cleanBuildVer = buildVer & 0x0FFFFFFFu;
  // Out-params are individually optional - skip the assignment if a caller
  // passed nullptr (e.g. they only care about the major version).
  if (major != nullptr) {
    *major = static_cast<unsigned int>(majorVer);
  }
  if (minor != nullptr) {
    *minor = static_cast<unsigned int>(minorVer);
  }
  if (build != nullptr) {
    *build = static_cast<unsigned int>(cleanBuildVer);
  }
  return true;
}

bool GetKernelNtVersion(UINT* major, UINT* minor, UINT* build, UINT* sp) {
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  if (hNtDll == nullptr) {
    return false;
  }
  const RtlGetVersion_t pfnRtlGetVersion =
      reinterpret_cast<RtlGetVersion_t>(GetProcAddress(hNtDll, "RtlGetVersion"));
  if (pfnRtlGetVersion == nullptr) {
    return false;
  }
  // Zero-init so the non-set members start clean (the API only writes the
  // ones it knows about for the size we passed). dwOSVersionInfoSize must
  // be exactly sizeof(OSVERSIONINFOEXW) or the call rejects the buffer.
  OSVERSIONINFOEXW osverinfo;
  SecureZeroMemory(&osverinfo, sizeof(OSVERSIONINFOEXW));
  osverinfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
  // RtlGetVersion returns NTSTATUS; STATUS_SUCCESS == 0. In practice it
  // never fails for a correctly-sized buffer, but checking the contract
  // beats trusting it.
  const NTSTATUS rtlStatus = pfnRtlGetVersion(&osverinfo);
  if (rtlStatus != STATUS_SUCCESS || osverinfo.dwMajorVersion == 0) {
    return false;
  }
  const UINT majorVer = static_cast<UINT>(osverinfo.dwMajorVersion);
  const UINT minorVer = static_cast<UINT>(osverinfo.dwMinorVersion);
  const UINT buildVer = static_cast<UINT>(osverinfo.dwBuildNumber);
  const UINT spMajor  = static_cast<UINT>(osverinfo.wServicePackMajor);
  if (is_dcheck) {
    LOG(DEBUG) << L"NT MajorVersion: " << logging::Hex(majorVer);
    LOG(DEBUG) << L"NT MinorVersion: " << logging::Hex(minorVer);
    LOG(DEBUG) << L"NT BuildNumber: " << logging::Hex(buildVer);
    LOG(DEBUG) << L"NT Service Pack: " << logging::Hex(spMajor);
  }
  if (major != nullptr) {
    *major = majorVer;
  }
  if (minor != nullptr) {
    *minor = minorVer;
  }
  if (build != nullptr) {
    *build = buildVer;
  }
  if (sp != nullptr) {
    *sp = spMajor;
  }
  return true;
}

bool GetUserNtVersion(UINT* major, UINT* minor, UINT* build, UINT* sp) {
  HMODULE hKernel32Dll = GetModuleHandleW(L"kernel32.dll");
  if (hKernel32Dll == nullptr) {
    return false;
  }
  const GetVersionExW_t pfnGetVersionExW =
      reinterpret_cast<GetVersionExW_t>(GetProcAddress(hKernel32Dll, "GetVersionExW"));
  if (pfnGetVersionExW == nullptr) {
    return false;
  }
  // Zero-init for the same reason as GetKernelNtVersion above.
  OSVERSIONINFOEXW osverinfo;
  SecureZeroMemory(&osverinfo, sizeof(OSVERSIONINFOEXW));
  osverinfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
  // GetVersionExW returns BOOL; 0 = failure. Combine with the dwMajorVersion
  // sanity check so a partially-failing call can't slip through.
  if (!pfnGetVersionExW(&osverinfo) || osverinfo.dwMajorVersion == 0) {
    return false;
  }
  const UINT majorVer = static_cast<UINT>(osverinfo.dwMajorVersion);
  const UINT minorVer = static_cast<UINT>(osverinfo.dwMinorVersion);
  const UINT buildVer = static_cast<UINT>(osverinfo.dwBuildNumber);
  const UINT spMajor  = static_cast<UINT>(osverinfo.wServicePackMajor);
  if (is_dcheck) {
    LOG(DEBUG) << L"User NT MajorVersion: " << logging::Hex(majorVer);
    LOG(DEBUG) << L"User NT MinorVersion: " << logging::Hex(minorVer);
    LOG(DEBUG) << L"User NT BuildNumber: " << logging::Hex(buildVer);
    LOG(DEBUG) << L"User NT Service Pack: " << logging::Hex(spMajor);
  }
  if (major != nullptr) {
    *major = majorVer;
  }
  if (minor != nullptr) {
    *minor = minorVer;
  }
  if (build != nullptr) {
    *build = buildVer;
  }
  if (sp != nullptr) {
    *sp = spMajor;
  }
  return true;
}

void LogOsInfo() {
  std::string winever;
  if (IsRunningOnWine(&winever)) {
    LOG(INFO) << L"Running on WINE " << winever.c_str();
  }
  UINT major = 0;
  UINT minor = 0;
  UINT build = 0;
  UINT sp    = 0;
  // Test raw version. Can't be spoofed, using undocumented function the MSVCRT uses.
  // RtlGetNtVersionNumbers does not return service-pack info.
  if (GetRawNtVersion(&major, &minor, &build)) {
    LOG(INFO) << L"RtlGetNtVersionNumbers NTVER: " << major << L"." << minor << L"." << build;
  }
  // Reset back to zero
  major = 0;
  minor = 0;
  build = 0;
  sp    = 0;
  // Test the "official" way to get "real" NT version numbers. Kernel mode.
  if (GetKernelNtVersion(&major, &minor, &build, &sp)) {
    LOG(INFO) << L"RtlGetVersion NTVER: " << major << L"." << minor << L"." << build << L" SP"
              << sp;
  }
  major = 0;
  minor = 0;
  build = 0;
  sp    = 0;
  // Test the legacy "official" way to get Windows version info from user mode.
  if (GetUserNtVersion(&major, &minor, &build, &sp)) {
    LOG(INFO) << L"GetVersionExW NTVER: " << major << L"." << minor << L"." << build << L" SP"
              << sp;
  }
}
