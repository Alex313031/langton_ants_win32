#include "utils.h"

#include <shlwapi.h>

#include "ants.h"
#include "globals.h"
#include "resource.h"

unsigned long g_default_speed = kHighSpeed;

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
// BMP file at the path the user chose. On success, if outSavedPath is non-null,
// the chosen path is written there so the caller can surface it to the user
// (e.g. via UserMessage). Pass nullptr to skip.
//
// BMP layout (no palette for 32-bit):
//   BITMAPFILEHEADER  (14 bytes) - magic 'BM', file size, pixel data offset
//   BITMAPINFOHEADER  (40 bytes) - dimensions, bit depth, compression
//   Pixel data        (w * h * 4 bytes) - 32-bit BGRA, bottom-up row order
bool SaveClientBitmap(HWND hWnd, std::wstring* outSavedPath) {
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
    if (outSavedPath != nullptr) {
      *outSavedPath = szFile;
    }
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
  UINT major = 0;
  UINT minor = 0;
  // Use the raw NT version: can't be spoofed by the manifest-driven shim that
  // GetVersionExW / RtlGetVersion go through, anything higher than 5.0 returns true.
  if (GetRawNtVersion(&major, &minor, nullptr)) {
    return major > 5u || (major == 5u && minor >= 1u);
  }
  return false; // Safe fallback, assume Win 2K
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
    return 0x0;
  }
  const std::wstring comctl32_path = std::wstring(systemDir) + L"\\" + kComCtl32Dll;

  HMODULE hComCtl32Dll = LoadLibraryW(comctl32_path.c_str());
  if (hComCtl32Dll == nullptr) {
    const DWORD error = GetLastError();
    LOG(ERROR) << L"Failed to load " << kComCtl32Dll << L", hComCtl32Dll was null! Error: "
               << logging::Hex(error);
    return 0x0;
  }

  DWORD dwVersion = 0x0;
  DLLGETVERSIONPROC pDllGetVersion =
      reinterpret_cast<DLLGETVERSIONPROC>(GetProcAddress(hComCtl32Dll, "DllGetVersion"));
  if (pDllGetVersion == nullptr) {
    const DWORD error = GetLastError();
    LOG(ERROR) << L"Failed to get DllGetVersion address. Error: " << logging::Hex(error);
    return 0x0;
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

bool ParseCommandLine(int argc, LPWSTR argv[]) {
  bool parsed;
  bool is_debug_mode   = false;
  bool is_version_mode = false;
  bool is_help_mode    = false;
  if (argv) {
    // argv[0] is the .exe path (CommandLineToArgvW convention); skip it so a
    // path containing characters that happen to match a flag literal can't
    // false-trigger one of the wcscmp checks below.
    for (int i = 1; i < argc; ++i) {
      wchar_t* arg = argv[i];
      is_debug_mode |= (wcscmp(arg, L"--debug") == 0) || (wcscmp(arg, L"-d") == 0) ||
                       (wcscmp(arg, L"-debug") == 0) || (wcscmp(arg, L"/d") == 0) ||
                       (wcscmp(arg, L"/D") == 0);
      is_version_mode |= (wcscmp(arg, L"--version") == 0) || (wcscmp(arg, L"-v") == 0) ||
                         (wcscmp(arg, L"-ver") == 0) || (wcscmp(arg, L"/v") == 0) ||
                         (wcscmp(arg, L"/V") == 0);
      is_help_mode |= (wcscmp(arg, L"--help") == 0) || (wcscmp(arg, L"-h") == 0) ||
                      (wcscmp(arg, L"-?") == 0) || (wcscmp(arg, L"/h") == 0) ||
                      (wcscmp(arg, L"/H") == 0) || (wcscmp(arg, L"/?") == 0);
    }
    parsed = true;
  } else {
    parsed = false;
  }
  if (is_version_mode && !is_help_mode) {
    g_show_version = true;
  }
  if (is_help_mode) {
    g_show_help = true;
  }
  if (is_debug_mode) {
    g_debug_mode = true;
  }
  return parsed;
}

int ShowVersionAndExit() {
  std::wcout << GetAppName() << L" Win32 Version " << GetVersionString() << std::endl;
  system("pause");
  return 0;
}

int ShowHelpAndExit() {
  std::wcout << L"\n " << ORIG_FILENAME << L" Usage: \n" << std::flush;
  std::wostringstream wostr;
  wostr << L"   /d | -d | --debug   : Enable debug logging\n"
        << L"   /v | -v | --version : Show version info \n"
        << L"   /? | -h | --help    : Show this Help \n"
        << std::flush;
  static const std::wstring kHelpMsg = wostr.str();
  std::wcout << kHelpMsg.c_str() << std::endl;
  system("pause");
  return 0;
}
