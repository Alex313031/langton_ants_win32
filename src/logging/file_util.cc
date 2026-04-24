#include "file_util.h"

#include <shlobj.h>

#include "check.h"

namespace logging {
  HANDLE g_log_file       = INVALID_HANDLE_VALUE;
  volatile bool file_open = false;
}

const std::wstring logging::GetCurrentRelDir() {
  wchar_t exe_path[MAX_PATH];
  HMODULE this_app = GetModuleHandleW(nullptr); // Get handle to whatever app is using this
  if (!this_app) {
    return std::wstring();
  }
  DWORD got_path = GetModuleFileNameW(this_app, exe_path, MAX_PATH);
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

// Usually %LOCALAPPDATA%\kProgName
const std::wstring logging::GetAppDataDir() {
  wchar_t kLocalAppData[MAX_PATH];
  HRESULT shAppData = SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, kLocalAppData);
  if (S_OK == shAppData) {
    const std::wstring log_dir = std::wstring(kLocalAppData) + L"\\" + kProgName + L"\\";
    CreateDirectoryW(log_dir.c_str(), nullptr); // Fails silently if exists
    return log_dir;
  } else {
    std::wcerr << L"Failed to get %LOCALAPPDATA%\\" << kProgName.c_str() << std::endl;
    return std::wstring();
  }
}

bool logging::WriteUTF16BOM(HANDLE hFile) {
  static const WORD wBOM = 0xFEFF;
  DWORD written          = 0;
  return WriteFile(hFile, &wBOM, static_cast<DWORD>(sizeof(WORD)), &written, nullptr);
}

const bool logging::ShouldTruncateLogFile() {
  return true; // TODO: Add more logic here.
}

bool logging::OpenFileForWriting(const std::wstring& logfile_path) {
  if (logfile_path.length() >= MAX_PATH) {
    return false;
  }
  CHECK(!file_open);
  bool write_bom                  = false;
  const bool should_truncate_file = ShouldTruncateLogFile();
  // Try to create a new file first, this will fail in an admin owned dir %PROGRAMFILES%
  // in which case we get ERROR_ACCESS_DENIED, and handle it below with OpenFileForWritingAlt.
  g_log_file = CreateFileW(logfile_path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,                     // Read/Write
                           FILE_SHARE_READ | FILE_SHARE_WRITE,               // Sharing permissions
                           nullptr,                                          // Default security
                           CREATE_NEW,                                       // Fail if file exists
                           FILE_ATTRIBUTE_ARCHIVE | FILE_FLAG_WRITE_THROUGH, // File write flags
                           nullptr);
  if (g_log_file == INVALID_HANDLE_VALUE) {
    DWORD err        = GetLastError();
    std::wstring msg = L"";
    if (err == ERROR_FILE_EXISTS) {
      const DWORD dwCreationFlag = should_truncate_file ? TRUNCATE_EXISTING : OPEN_EXISTING;
      // File exists, truncate and then open it for appending. This also works if the file already
      // exists in an admin owned dir like %PROGRAMFILES%. It won't work for a truly inaccessible
      // directory, but works in our case when the program is installed in %PROGRAMFILES%.
      g_log_file = CreateFileW(logfile_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, dwCreationFlag,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

      if (g_log_file == INVALID_HANDLE_VALUE) {
        msg = L"Failed to open existing file. Error = " + std::to_wstring(GetLastError());
        MessageBoxW(nullptr, msg.c_str(), L"Open File Error", MB_OK | MB_ICONERROR);
        file_open = false;
        return false;
      } else {
        file_open = true;
        write_bom = should_truncate_file;
        if (!should_truncate_file && dwCreationFlag == OPEN_EXISTING) {
          // Move to end of file for append mode
          if (SetFilePointer(g_log_file, 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER &&
              GetLastError() != NO_ERROR) {
            msg = L"Failed to seek to end of file. Error = " + std::to_wstring(GetLastError());
            MessageBoxW(nullptr, msg.c_str(), L"SetFilePointer Error", MB_OK | MB_ICONERROR);
            CloseFileHandle();
            return false;
          }
        }
      }
    } else if (err == ERROR_ACCESS_DENIED) {
      // We can't write to current dir due to admin owned dir or permissions,
      // so try to write logfile to localappdata dir.
      const std::wstring alt_logfile_path = GetAppDataDir() + kLogFileName;
      file_open = OpenFileForWritingAlt(alt_logfile_path, should_truncate_file, write_bom);
    } else {
      msg = L"Failed to open file for writing. Error = " + std::to_wstring(GetLastError());
      MessageBoxW(nullptr, msg.c_str(), L"Open File Error", MB_OK | MB_ICONERROR);
      file_open = false;
      return false;
    }
  } else {
    if (GetIsConsoleAttached()) {
      std::wcout << L"Note: Creating new log file with UTF-16 BOM: " << logfile_path << std::endl;
    }
    file_open = true;
    write_bom = true;
  }
  if (file_open && write_bom) {
    return WriteUTF16BOM(g_log_file);
  }

  return file_open;
}

// Handles writing logfile to alternate path, usually GetAppDataDir dir.
bool logging::OpenFileForWritingAlt(const std::wstring& alt_logfile_path,
                                    bool should_truncate,
                                    bool& out_write_bom) {
  out_write_bom = false;
  if (alt_logfile_path.length() >= MAX_PATH) {
    return false;
  }
  CHECK(!file_open);
  g_log_file = CreateFileW(alt_logfile_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_ARCHIVE | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (g_log_file == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_EXISTS) {
      const DWORD dwCreationFlag = should_truncate ? TRUNCATE_EXISTING : OPEN_EXISTING;
      g_log_file = CreateFileW(alt_logfile_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, dwCreationFlag,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
      if (g_log_file != INVALID_HANDLE_VALUE) {
        out_write_bom = should_truncate;
        if (!should_truncate && dwCreationFlag == OPEN_EXISTING) {
          if (SetFilePointer(g_log_file, 0, nullptr, FILE_END) == INVALID_SET_FILE_POINTER &&
              GetLastError() != NO_ERROR) {
            const std::wstring msg =
                L"Failed to seek to end of file. Error = " + std::to_wstring(GetLastError());
            MessageBoxW(nullptr, msg.c_str(), L"SetFilePointer Error", MB_OK | MB_ICONERROR);
            out_write_bom = false;
            CloseFileHandle();
            return false;
          }
        }
      }
    }
  } else {
    if (GetIsConsoleAttached()) {
      std::wcout << L"Note: Creating new log file with UTF-16 BOM: " << alt_logfile_path
                 << std::endl;
    }
    out_write_bom = true; // New file always needs BOM
  }
  return g_log_file != INVALID_HANDLE_VALUE;
}

// Closes file handle, if it exists.
bool logging::CloseFileHandle() {
  bool closed = false;
  CHECK(g_log_file != INVALID_HANDLE_VALUE);
  HANDLE kFileHandle             = g_log_file;
  const std::wstring this_handle = std::to_wstring(reinterpret_cast<long long>(kFileHandle));
  if (g_log_file != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(g_log_file);
    closed = CloseHandle(g_log_file);
  }
  if (closed) {
    g_log_file = INVALID_HANDLE_VALUE;
    file_open  = false;
    std::wcerr << L"[DEBUG] Closed file handle " << this_handle.c_str() << std::endl;
  } else {
    const std::wstring msg = L"Failed to close file handle " + this_handle;
    MessageBoxW(nullptr, msg.c_str(), L"CloseFileHandle Error", MB_OK | MB_ICONERROR);
  }
  return closed;
}

bool logging::AppendTextToFile(const std::wstring& log_line) {
  if (!IsFileOpen() || !logging_initialized) {
    return false;
  }

  // Append newline to the log line
  std::wstring line_with_newline = log_line + L"\r\n";

  // Write UTF-16 LE directly (matches the FF FE BOM written on file creation)
  const DWORD byte_count = static_cast<DWORD>(line_with_newline.length() * sizeof(wchar_t));
  DWORD bytes_written    = 0;
  BOOL result =
      WriteFile(g_log_file, line_with_newline.c_str(), byte_count, &bytes_written, nullptr);

  if (result && (bytes_written == byte_count)) {
    return FlushFileBuffers(g_log_file);
  }
  return false;
}

bool logging::ClearFileContents() {
  if (!IsFileOpen() || !logging_initialized) {
    return false;
  }

  // Flush any pending writes before truncating
  FlushFileBuffers(g_log_file);

  // Move to beginning of file
  if (SetFilePointer(g_log_file, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
      GetLastError() != NO_ERROR) {
    return false;
  }

  // Truncate file at current position (beginning)
  if (!SetEndOfFile(g_log_file)) {
    return false;
  }

  return true;
}

bool logging::IsFileOpen() {
  if (g_log_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  return file_open;
}
