#include "utils.h"

#include "ants.h"
#include "globals.h"
#include "resource.h"

volatile bool g_playsound = false;

// The toolbar child window handle. Kept file-static so nothing else can
// accidentally mutate it — other TUs interact only via the functions below.
static HWND s_hToolbar = nullptr;

// Saved original toolbar WndProc so our subclass can chain through to it.
static WNDPROC s_origToolbarProc = nullptr;

// Measured in pixels after TB_AUTOSIZE runs. Exposed through globals.h so
// ants.cc / main.cc can offset the back-buffer blit and mouse coords by it.
int g_toolbarHeight = 0;

unsigned long g_default_speed = kHyperSpeed;

// Bitmap indices captured from TB_ADDBITMAP for the dynamic-icon buttons.
// TB_ADDBITMAP returns the starting index of images it added to the toolbar's
// internal image list, which is what TBBUTTON::iBitmap (and TBBUTTONINFO::
// iImage) references. SetPauseButton / SetSoundButton toggle between these
// to flip the icon on state changes.
static int s_idxPause  = 0;
static int s_idxPlay   = 0;
static int s_idxSound  = 0;
static int s_idxMute   = 0;
static int s_idxAnts   = 0;
static int s_idxSpeed  = 0;

// Reads the CHECKED state of every menu group at startup and sets the
// corresponding globals. This makes all defaults entirely RC-driven: changing
// which item has CHECKED in langton_ants.rc is the only code change needed to
// alter a default setting.
void InitMenuDefaults(HWND hWnd) {
  HMENU hMenu     = GetMenu(hWnd);
  HMENU hSettings = GetSubMenu(hMenu, 1);
  HMENU hConc     = GetSubMenu(hSettings, 3); // Num Ants submenu
  HMENU hDelay    = GetSubMenu(hSettings, 5); // Speed menu
  HMENU hBkgMenu  = GetSubMenu(hSettings, 8); // Background color menu

  // Background color
  const struct { UINT id; COLORREF color; } bkgs[] = {
    { IDM_WHITE_BKG, RGB_WHITE },
    { IDM_BLACK_BKG, RGB_BLACK },
    { IDM_GREY_BKG,  RGB_GREY },
    { IDM_RED_BKG,   RGB_RED   },
    { IDM_GREEN_BKG, RGB_GREEN },
    { IDM_BLUE_BKG,  RGB_BLUE  },
  };
  for (const auto& b : bkgs) {
    if (GetMenuState(hBkgMenu, b.id, MF_BYCOMMAND) & MF_CHECKED) {
      g_bkg_color = b.color;
      break;
    }
  }

  // Draw delay
  const struct { UINT id; unsigned long ms; } delays[] = {
    { IDM_SLOW,   kSlowSpeed },
    { IDM_MEDIUM, kMedSpeed },
    { IDM_FAST,   kHighSpeed },
    { IDM_HYPER,  kHyperSpeed },
  };
  for (const auto& d : delays) {
    if (GetMenuState(hDelay, d.id, MF_BYCOMMAND) & MF_CHECKED) {
      g_delay = d.ms;
      g_default_speed = d.ms;
      break;
    }
  }

  // Concurrent ants — exactly one IDM_CONC_N must be CHECKED in the RC.
  // IDs are consecutive (IDM_CONC_1..IDM_CONC_8) so we can probe them in a loop.
  for (UINT id = IDM_CONC_1; id <= IDM_CONC_8; ++id) {
    if (GetMenuState(hConc, id, MF_BYCOMMAND) & MF_CHECKED) {
      SetNumAnts((id - IDM_CONC_1) + 1);
      break;
    }
  }

  // Monochrome toggle — grey out chromatic bg items and override the RC's
  // bg CHECKED to grey (monochrome defaults to grey bg + white ants
  // regardless of what the RC otherwise selected; white and black remain
  // selectable afterward).
  if (GetMenuState(hSettings, IDM_MONOCHROME, MF_BYCOMMAND) & MF_CHECKED) {
    g_monochrome = true;
    EnableMenuItem(hBkgMenu, IDM_RED_BKG,   MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_GREEN_BKG, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(hBkgMenu, IDM_BLUE_BKG,  MF_BYCOMMAND | MF_GRAYED);
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
//   BITMAPFILEHEADER  (14 bytes) — magic 'BM', file size, pixel data offset
//   BITMAPINFOHEADER  (40 bytes) — dimensions, bit depth, compression
//   Pixel data        (w * h * 4 bytes) — 32-bit BGRA, bottom-up row order
bool SaveClientBitmap(HWND hWnd) {
  // Prompt the user for a destination path
  wchar_t szFile[MAX_PATH]  = {};
  OPENFILENAMEW ofn         = {};
  ofn.lStructSize  = sizeof(OPENFILENAMEW);
  ofn.hwndOwner    = hWnd;
  ofn.lpstrFile    = szFile;
  ofn.nMaxFile     = MAX_PATH;
  ofn.lpstrFilter  = L"Bitmap Files (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrDefExt  = L"bmp";
  ofn.lpstrTitle   = L"Save Bitmap As";
  ofn.Flags        = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

  if (!GetSaveFileNameW(&ofn)) {
    return false; // user cancelled or dialog error
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
  bi.biSize        = sizeof(BITMAPINFOHEADER);
  bi.biWidth       = width;
  bi.biHeight      = height;
  bi.biPlanes      = 1;
  bi.biBitCount    = 32;
  bi.biCompression = BI_RGB;
  bi.biSizeImage   = static_cast<DWORD>(width * height * 4);

  // GetDIBits copies the selected bitmap's pixels into our buffer in the format
  // described by bi. With BI_RGB and 32 bits, each pixel is 4 bytes: BGRA
  // (GDI leaves the alpha byte as 0, which is fine for BMP).
  std::vector<BYTE> pixels(bi.biSizeImage);
  GetDIBits(g_hdcMem, g_hbmMem, 0, static_cast<UINT>(height), pixels.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

  LeaveCriticalSection(&g_paintCS);

  // Build the BMP file header
  const DWORD pixelDataOffset = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  BITMAPFILEHEADER bf = {};
  bf.bfType    = 0x4D42; // 'BM' signature
  bf.bfSize    = pixelDataOffset + bi.biSizeImage;
  bf.bfOffBits = pixelDataOffset;

  // Write the three sections to the file
  HANDLE hFile = CreateFileW(szFile, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD written;
  const bool ok =
      WriteFile(hFile, &bf,           sizeof(bf),      &written, nullptr) &&
      WriteFile(hFile, &bi,           sizeof(bi),      &written, nullptr) &&
      WriteFile(hFile, pixels.data(), bi.biSizeImage,  &written, nullptr);

  CloseHandle(hFile);
  return ok;
}

inline static void __KillInt3Asm() {
#ifdef __MINGW32__
  asm("int3\n\t"
      "ud2");
#else
  __asm int 3 // Execute int3 interrupt
  __asm {
    UD2
  } // Execute 0x0F, 0x0B
#endif // __MINGW32__
}

void TestTrap() {
  StopPlayWav();
  PlayWavFile(L"fahh.wav", false);
  __KillInt3Asm();
  return;
}

// --- Background music via MCI ---------------------------------------------
//
// We drive background playback through MCI (Media Control Interface) rather
// than the simpler PlaySound API. Reason: Windows 2000's PlaySound cannot
// decode non-PCM WAV files reliably — a PlaySoundW call with an MS ADPCM
// file (the compact codec we use for ants.wav to keep the shipped file
// small) returns TRUE but silently drops the audio. Win2K's sndrec32 plays
// the exact same file fine; sndrec32 goes through MCI, and MCI's ACM
// interaction is the one that actually works cross-version.
//
// MCI takes plain ASCII-style command strings like `open <path> alias <name>`
// or `play <name> repeat`. mciSendStringW parses these and dispatches them
// to the appropriate MCI device driver (the waveform driver, in our case).
// Each command returns an MCIERROR (0 == success). A process-global alias
// ("langton_ants_bgm") names our open device instance so stop/close can find
// it again later.
//
// Using MCI also makes a future pause/resume feature trivial: the waveform
// driver supports `pause <alias>` and `resume <alias>` natively, preserving
// playback position — something PlaySound cannot do.

static const wchar_t kMciBgmAlias[] = L"langton_ants_bgm";
// Whether we currently hold the waveform device open under kMciBgmAlias.
// Avoids issuing commands against a non-existent alias (which MCI would
// answer with a confusing "device not open" error).
static bool s_mciOpened = false;
// Path of the temp .wav we materialized from IDR_BGM_WAVE (empty when the
// current session is playing from a side-by-side file instead). Populated
// on the first embedded-source PlayWavFile call; StopPlayWav removes the
// file from disk so we don't leak temp files across runs.
static std::wstring s_embeddedTempPath;

// Small helper: convert an MCIERROR code into a human-readable string using
// mciGetErrorStringW, the MCI-provided lookup. Used only for logging.
static std::wstring MciErrText(MCIERROR err) {
  wchar_t buf[256] = {};
  mciGetErrorStringW(err, buf, sizeof(buf) / sizeof(buf[0]));
  return std::wstring(buf);
}

// Materializes the IDR_BGM_WAVE resource to a file in the user's temp
// directory and returns that path (empty on failure). MCI's string API can
// only open named files — there is no "play from memory" form — so when
// we want to play the embedded WAV we have to stage it on disk first.
// Windows' FindResource returns a pointer to the PE-loaded resource bytes
// without extra allocation; we just WriteFile them out.
static std::wstring ExtractEmbeddedWavToTemp() {
  HRSRC hRsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_BGM_WAVE), L"WAVE");
  if (hRsrc == nullptr) {
    LOG(ERROR) << L"FindResource IDR_BGM_WAVE failed";
    return std::wstring();
  }
  HGLOBAL hGlob = LoadResource(nullptr, hRsrc);
  if (hGlob == nullptr) {
    LOG(ERROR) << L"LoadResource IDR_BGM_WAVE failed";
    return std::wstring();
  }
  const LPVOID pData = LockResource(hGlob);
  const DWORD size = SizeofResource(nullptr, hRsrc);
  if (pData == nullptr || size == 0) {
    LOG(ERROR) << L"LockResource/SizeofResource returned empty for IDR_BGM_WAVE";
    return std::wstring();
  }

  // GetTempPathW always returns a path with a trailing backslash, so we can
  // concatenate without an explicit separator.
  wchar_t tempDir[MAX_PATH] = {};
  DWORD n = GetTempPathW(MAX_PATH, tempDir);
  if (n == 0 || n > MAX_PATH) {
    LOG(ERROR) << L"GetTempPathW failed";
    return std::wstring();
  }
  std::wstring tempPath = std::wstring(tempDir) + L"langton_ants_bgm.wav";

  HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << L"CreateFile temp failed for " << tempPath;
    return std::wstring();
  }
  DWORD written = 0;
  BOOL ok = WriteFile(hFile, pData, size, &written, nullptr);
  CloseHandle(hFile);
  if (!ok || written != size) {
    LOG(ERROR) << L"WriteFile temp WAV short-write: " << written << L"/" << size;
    DeleteFileW(tempPath.c_str());
    return std::wstring();
  }
  return tempPath;
}

bool PlayWavFile(const std::wstring& wav_file, bool use_embedded) {
  // File-source path still requires a filename; embedded source ignores it.
  if (!use_embedded && wav_file.empty()) {
    return false;
  }
  // If the device is already open, this call is a "resume from paused"
  // rather than a fresh play. Issue MCI `resume` to continue from the
  // current playback position — do NOT re-open (that would reset position
  // to 0) and do NOT re-issue `play` (the original play's notify is still
  // pending for the looping handler, and a fresh `play` would start over).
  if (s_mciOpened) {
    MCIERROR err = mciSendStringW(L"resume langton_ants_bgm", nullptr, 0, nullptr);
    if (err != 0) {
      LOG(ERROR) << L"MCI resume failed: " << MciErrText(err);
      g_playsound = false;
      return false;
    }
    g_playsound = true;
    return true;
  }

  // First-time open + play from position 0. `file` is the path MCI opens;
  // either the user-supplied name resolved against the exe dir, or the
  // temp file we materialize from the embedded IDR_BGM_WAVE resource.
  std::wstring file;
  if (use_embedded) {
    file = ExtractEmbeddedWavToTemp();
    if (file.empty()) {
      return false;
    }
    s_embeddedTempPath = file;
  } else {
    const std::wstring cwd = GetExeDir();
    if (cwd.empty()) {
      return false;
    }
    file = cwd + wav_file;
  }
  // Quoting the path keeps spaces or punctuation in the exe dir from
  // splitting the command into separate tokens. `type waveaudio` pins the
  // open to the waveform driver regardless of the file extension, making
  // the open deterministic.
  std::wstring openCmd = L"open \"";
  openCmd += file;
  openCmd += L"\" type waveaudio alias langton_ants_bgm";
  MCIERROR err = mciSendStringW(openCmd.c_str(), nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI open failed for " << file << L": " << MciErrText(err);
    if (!s_embeddedTempPath.empty()) {
      DeleteFileW(s_embeddedTempPath.c_str());
      s_embeddedTempPath.clear();
    }
    g_playsound = false;
    return false;
  }
  s_mciOpened = true;

  // `notify` asks the waveform driver to send MM_MCINOTIFY when playback
  // completes. mciSendStringW's FOURTH parameter is the callback HWND for
  // exactly this purpose — passing nullptr makes MCI silently drop the
  // notification, breaking the loop. We use mainHwnd so the notification
  // reaches our WindowProc's MM_MCINOTIFY handler.
  //
  // MCIWAVE's `play` does NOT support a `repeat` parameter (unlike MCIAVI
  // and MCICDA), so looping is done manually: on every successful completion
  // the handler re-issues `play ... from 0 notify` against the same callback.
  err = mciSendStringW(L"play langton_ants_bgm notify", nullptr, 0, mainHwnd);
  if (err != 0) {
    LOG(ERROR) << L"MCI play failed: " << MciErrText(err);
    mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
    s_mciOpened = false;
    if (!s_embeddedTempPath.empty()) {
      DeleteFileW(s_embeddedTempPath.c_str());
      s_embeddedTempPath.clear();
    }
    g_playsound = false;
    return false;
  }
  g_playsound = true;
  return true;
}

bool PauseWavFile() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // `pause` suspends playback without resetting the position cursor — the
  // pending `notify` registration from the prior `play ... notify` stays
  // alive so that once PlayWavFile-as-resume is called and the clip finally
  // reaches its end, MM_MCINOTIFY still fires and the looping handler kicks
  // in. Critically we do NOT send `stop` or `close` here; those would drop
  // the position and the device instance respectively.
  MCIERROR err = mciSendStringW(L"pause langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI pause failed: " << MciErrText(err);
  }
  g_playsound = false;
  return true;
}

bool StopPlayWav() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // Full tear-down path (used by ShutDownApp). Stop halts playback; close
  // releases the waveform device so the next open can succeed cleanly (e.g.
  // if the app is re-launched within the same user session). The mute
  // toggle does NOT come through here — it uses PauseWavFile instead so
  // that the next unmute resumes from the pause point rather than
  // restarting from 0.
  MCIERROR err = mciSendStringW(L"stop langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI stop failed: " << MciErrText(err);
  }
  err = mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI close failed: " << MciErrText(err);
  }
  s_mciOpened = false;
  g_playsound = false;
  // If this session materialized the embedded WAV to a temp file, delete
  // it now so we don't litter %TEMP% with a stale copy across app runs.
  if (!s_embeddedTempPath.empty()) {
    DeleteFileW(s_embeddedTempPath.c_str());
    s_embeddedTempPath.clear();
  }
  return true;
}

bool ToggleSound() {
  if (g_playsound) {
    return PauseWavFile();
  } else {
    return PlayWavFile(sound_file, kUseEmbeddedBgm);
  }
}

bool InfoBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  HWND hWndTmp;
  if (hWnd == nullptr && mainHwnd != nullptr) {
    hWndTmp = mainHwnd;
  } else {
    hWndTmp = hWnd;
  }
  return (MessageBoxW(hWndTmp, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION) == IDOK);
}

bool WarnBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  HWND hWndTmp;
  if (hWnd == nullptr && mainHwnd != nullptr) {
    hWndTmp = mainHwnd;
  } else {
    hWndTmp = hWnd;
  }
  return (MessageBoxW(hWndTmp, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING) == IDOK);
}

bool ErrorBox(HWND hWnd, const std::wstring& title, const std::wstring& message) {
  HWND hWndTmp;
  if (hWnd == nullptr && mainHwnd != nullptr) {
    hWndTmp = mainHwnd;
  } else {
    hWndTmp = hWnd;
  }
  return (MessageBoxW(hWndTmp, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR) == IDOK);
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
// which gives every button a permanent 3D raised bevel — the look consistent
// with the Win2000/XP-Classic appearance across every Windows version.
//
// We bind dynamically rather than linking uxtheme.lib so the binary still
// loads on Windows 2000 (where uxtheme.dll does not exist). If LoadLibrary
// fails there is nothing to disable anyway — classic rendering is already
// in effect — so we just return quietly.
static void DisableWindowTheme(HWND hWnd) {
  HMODULE hUxTheme = LoadLibraryW(L"uxtheme.dll");
  if (hUxTheme == nullptr) return;
  typedef HRESULT (WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
  SetWindowThemeFn pSetWindowTheme = reinterpret_cast<SetWindowThemeFn>(
      GetProcAddress(hUxTheme, "SetWindowTheme"));
  if (pSetWindowTheme != nullptr) {
    // Empty strings (not nullptr) mean "use no theme" for this window.
    pSetWindowTheme(hWnd, L"", L"");
  }
  FreeLibrary(hUxTheme);
}

// Subclass for the toolbar, handling two things:
//
//   WM_ERASEBKGND — fill the client area with the standard 3D face color.
//     On real Windows this is redundant (the opaque toolbar paints its own
//     background during WM_PAINT anyway), but Wine's toolbar does not
//     reliably fill the background, leaving the control transparent.
//     Painting it here covers Wine without changing anything on real Windows.
//
//   WM_PAINT — chain to the original proc so it draws buttons/background,
//     then draw a single-pixel raised line along the bottom via
//     DrawEdge(BDR_RAISEDOUTER, BF_BOTTOM). This gives the classic
//     early-2000s Win32 separator between toolbar and the content area
//     below. The edge must be drawn AFTER the original paint because the
//     original's button rendering would otherwise overwrite it.
static LRESULT CALLBACK ToolbarSubclassProc(HWND hWnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam) {
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
    // cycle that the original proc used — that's fine, we just need to
    // draw a few lines and release.
    HDC hdc = GetDC(hWnd);
    if (hdc != nullptr) {
      RECT rc;
      GetClientRect(hWnd, &rc);
      // BDR_RAISEDINNER — single-pixel highlight/shadow, subtler than BDR_RAISEDOUTER
      // BF_BOTTOM restricts drawing to one edge.
      DrawEdge(hdc, &rc, BDR_RAISEDINNER | BDR_RAISEDOUTER, BF_BOTTOM);

      // Classic (non-FLAT) toolbars render TBSTYLE_SEP entries as blank
      // gaps rather than visible dividers. Walk the button list ourselves
      // and stamp an etched vertical line into each separator's rect so
      // groups stay visually distinct.
      const int count = static_cast<int>(
          SendMessageW(hWnd, TB_BUTTONCOUNT, 0, 0));
      for (int i = 0; i < count; i++) {
        TBBUTTON btn = {};
        if (!SendMessageW(hWnd, TB_GETBUTTON, i,
                         reinterpret_cast<LPARAM>(&btn))) continue;
        if (!(btn.fsStyle & TBSTYLE_SEP)) continue;
        RECT ir;
        if (!SendMessageW(hWnd, TB_GETITEMRECT, i,
                         reinterpret_cast<LPARAM>(&ir))) continue;
        // A 2-pixel-wide rect centered in the separator, inset vertically
        // by a couple pixels so the line doesn't touch the toolbar edges.
        const int xMid = (ir.left + ir.right) / 2;
        RECT lineRect = { xMid - 1, ir.top + 2, xMid + 1, ir.bottom - 2 };
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
// pull their images from a "bitmap strip" — a single wide bitmap where each
// button's image is a fixed-size slice. All of this app's toolbar icons are
// loaded from its own resources (IDB_* bitmaps in langton_ants.rc) rather than
// from the comctl32 standard strip, so the look stays consistent across
// Windows versions and theme variations.
//
// Button clicks arrive as WM_COMMAND messages to the parent, with wParam
// low-word set to the button's idCommand. Here we map the save button to
// IDM_SAVE_AS so it shares the existing menu handler — no duplicate code.
void CreateAppToolbar(HWND hParent, HINSTANCE hInst) {
  // Styles note — we deliberately do NOT use TBSTYLE_FLAT here. Per MSDN it
  // makes the toolbar transparent, meaning the parent is responsible for
  // painting the background. With WS_CLIPCHILDREN on our main window (which
  // we need to keep parent painting out of the toolbar's rect), there is
  // nothing to paint the background, so the area renders as whatever is in
  // the surface — desktop on Win2000, black on XP+ under DWM. Without
  // TBSTYLE_FLAT the toolbar is opaque: it paints its own background, which
  // the theme engine on XP+ handles automatically (themed raised look), and
  // Win2000 falls back to classic 3D raised shading.
  //
  // TBSTYLE_TOOLTIPS — show tooltip popups when the cursor hovers.
  // CCS_TOP is the default (toolbar docks to top of parent) so we omit it.
  HWND hTB = CreateWindowExW(
      0, TOOLBARCLASSNAME, nullptr,
      WS_CHILD | TBSTYLE_TOOLTIPS,
      0, 0, CW_USEDEFAULT, CW_USEDEFAULT,
      hParent, nullptr, hInst, nullptr);
  if (hTB == nullptr) {
    return;
  }

  // Tell the control which TBBUTTON layout we compiled against so it can
  // adapt if this binary runs against a different Common Controls DLL version.
  SendMessageW(hTB, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

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
  tbab.hInst = hInst;
  tbab.nID = IDB_SAVE_BMP;
  const int idxSave = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_PAUSE_BMP;
  s_idxPause = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_PLAY_BMP;
  s_idxPlay = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_ANTS_BMP;
  s_idxAnts = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_TIME_BMP;
  s_idxSpeed = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_SOUND_BMP;
  s_idxSound = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_MUTE_BMP;
  s_idxMute = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));
  tbab.nID = IDB_EXIT_BMP;
  const int idxExit = static_cast<int>(
      SendMessageW(hTB, TB_ADDBITMAP, 1, reinterpret_cast<LPARAM>(&tbab)));

  // --- Buttons -------------------------------------------------------------
  // TBBUTTON fields:
  //   iBitmap   — index into the loaded image list (ignored for TBSTYLE_SEP)
  //   idCommand — WM_COMMAND id sent when the button is clicked
  //   fsState   — TBSTATE_ENABLED for clickable (0 for separators)
  //   fsStyle   — TBSTYLE_BUTTON (push button) or TBSTYLE_SEP (gap)
  //   dwData    — app-defined extra data we don't need
  //   iString   — tooltip/label text pointer (cast through INT_PTR)
  TBBUTTON tbButtons[12] = {};

  tbButtons[0].fsStyle   = TBSTYLE_SEP;

  tbButtons[1].iBitmap   = idxSave;
  tbButtons[1].idCommand = IDM_SAVE_AS;
  tbButtons[1].fsState   = TBSTATE_ENABLED;
  tbButtons[1].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[1].iString   = reinterpret_cast<INT_PTR>(L"Save As");

  tbButtons[2].fsStyle   = TBSTYLE_SEP;

  tbButtons[3].iBitmap   = s_idxPause;
  tbButtons[3].idCommand = IDM_PAUSED;
  tbButtons[3].fsState   = TBSTATE_ENABLED;
  tbButtons[3].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[3].iString   = reinterpret_cast<INT_PTR>(L"Pause");

  tbButtons[4].fsStyle   = TBSTYLE_SEP;

  // Num Ants and Speed are deliberately adjacent with no separator — they
  // form a single "simulation knobs" pair.
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

  tbButtons[7].fsStyle   = TBSTYLE_SEP;

  tbButtons[8].iBitmap   = s_idxSound;
  tbButtons[8].idCommand = IDM_SOUND;
  tbButtons[8].fsState   = TBSTATE_ENABLED;
  tbButtons[8].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[8].iString   = reinterpret_cast<INT_PTR>(L"Sound");

  tbButtons[9].fsStyle   = TBSTYLE_SEP;

  tbButtons[10].iBitmap   = idxExit;
  tbButtons[10].idCommand = IDM_EXIT;
  tbButtons[10].fsState   = TBSTATE_ENABLED;
  tbButtons[10].fsStyle   = TBSTYLE_BUTTON;
  tbButtons[10].iString   = reinterpret_cast<INT_PTR>(L"Exit");

  tbButtons[11].fsStyle  = TBSTYLE_SEP;

  SendMessageW(hTB, TB_ADDBUTTONS,
              sizeof(tbButtons) / sizeof(tbButtons[0]),
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
      SetWindowLongPtrW(hTB, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(ToolbarSubclassProc)));

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
  RECT tbRect;
  GetWindowRect(hTB, &tbRect);
  g_toolbarHeight = tbRect.bottom - tbRect.top;
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
// via const_cast is safe — the control won't mutate the memory we point at.
void SetPauseButton(bool paused) {
  if (s_hToolbar == nullptr) return;
  TBBUTTONINFOW bi = {};
  bi.cbSize  = sizeof(bi);
  bi.dwMask  = TBIF_IMAGE | TBIF_TEXT;
  bi.iImage  = paused ? s_idxPlay : s_idxPause;
  bi.pszText = const_cast<LPWSTR>(paused ? L"Resume" : L"Pause");
  SendMessageW(s_hToolbar, TB_SETBUTTONINFOW, IDM_PAUSED,
              reinterpret_cast<LPARAM>(&bi));
}

void SetSoundButton(bool playing) {
  if (s_hToolbar == nullptr) return;
  TBBUTTONINFOW bi = {};
  bi.cbSize  = sizeof(bi);
  bi.dwMask  = TBIF_IMAGE | TBIF_TEXT;
  bi.iImage  = playing ? s_idxMute : s_idxSound;
  bi.pszText = const_cast<LPWSTR>(playing ? L"Mute" : L"Sound");
  SendMessageW(s_hToolbar, TB_SETBUTTONINFOW, IDM_SOUND,
              reinterpret_cast<LPARAM>(&bi));
}

void PopupUnderToolbarButton(HWND hOwner, int idCommand, HMENU hMenu) {
  if (s_hToolbar == nullptr || hMenu == nullptr) return;
  // TB_GETRECT returns the button's rect in toolbar-client coords.
  RECT rc;
  if (!SendMessageW(s_hToolbar, TB_GETRECT, idCommand,
                   reinterpret_cast<LPARAM>(&rc))) {
    return;
  }
  // Convert the bottom-left corner to screen space — that's where
  // TrackPopupMenu wants its anchor.
  POINT pt = { rc.left, rc.bottom };
  ClientToScreen(s_hToolbar, &pt);
  TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN,
                 pt.x, pt.y, 0, hOwner, nullptr);
}

bool HandleToolbarTooltips(NMHDR* pnmh) {
  if (pnmh == nullptr || s_hToolbar == nullptr) return false;
  // TTN_GETDISPINFOW and TTN_NEEDTEXTW have the same numeric value; accepting
  // both keeps us portable across comctl32 versions. For ANSI comctl the code
  // would be TTN_NEEDTEXTA, but this app is Unicode-only so we ignore that.
  if (pnmh->code != TTN_GETDISPINFOW && pnmh->code != TTN_NEEDTEXTW) {
    return false;
  }
  NMTTDISPINFOW* pdi = reinterpret_cast<NMTTDISPINFOW*>(pnmh);
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
      text = L"Change crawl speed";
      break;
    case IDM_PAUSED:
      text = g_paused ? L"Resume Ants" : L"Pause Ants";
      break;
    case IDM_SOUND:
      text = g_playsound ? L"Mute Background Sounds" : L"Play Background Sounds";
      break;
    default: return false; // unknown button — let the default handling run
  }
  pdi->lpszText = const_cast<LPWSTR>(text);
  return true;
}
