#include "cpu.h"

#include <cmath>

#include "globals.h"
#include "resource.h"
#include "utils.h"

static GetNativeSystemInfo_t pfnGetNativeSystemInfo = nullptr;

// GetSystemTimes is XP+. Dynamic-load with a fallback to "no system idle
// available" on Windows 2000, where the symbol simply doesn't exist in
// kernel32. NtQuerySystemInformation could fill the gap but the per-process
// numbers are the load-bearing ones - idle on 2000 just reports 0 instead of
// dragging in winternl + a SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION walk.
typedef BOOL(WINAPI* GetSystemTimes_t)(LPFILETIME lpIdleTime,
                                       LPFILETIME lpKernelTime,
                                       LPFILETIME lpUserTime);
static GetSystemTimes_t pfnGetSystemTimes = nullptr;

// Set by GetLogicalProcessorCount
int g_num_cpus = 0;

// 1.0 second delay between each CPU performance update.
const unsigned int g_perf_delay = 1000u;

// Our global performance snapshot
PerfSnapshot g_snapshot = {};

static HMODULE hKernel32 = nullptr;

// Per-sample baseline for delta computation. GetCPUPercent compares the
// current FILETIMEs against these to compute % CPU used over the interval
// since the previous tick. s_haveLastSample is false until the first call
// has populated them (that call returns 0 - no delta yet).
static FILETIME s_lastWall       = {};
static FILETIME s_lastProcKernel = {};
static FILETIME s_lastProcUser   = {};
static FILETIME s_lastSysIdle    = {};
static FILETIME s_lastSysKernel  = {};
static FILETIME s_lastSysUser    = {};
static bool s_haveLastSample     = false;

// FILETIME -> uint64 in 100-ns units. Saves repeating the ULARGE_INTEGER
// dance at every comparison site. FILETIME is 64-bit since 1601, so no
// overflow concern.
static inline ULONGLONG FileTimeToU64(const FILETIME& ft) {
  ULARGE_INTEGER ul;
  ul.LowPart  = ft.dwLowDateTime;
  ul.HighPart = ft.dwHighDateTime;
  return ul.QuadPart;
}

// Choose function depending on Windows version, return number of logical CPUs.
DWORD cpu::GetLogicalProcessorCount() {
  SYSTEM_INFO sysInfo;
  std::wstring whichfunc;
  if (!hKernel32) {
    LOG(ERROR) << L"Failed to get kernel32.dll!";
    return static_cast<DWORD>(0x00000000);
  } else {
    // Dynamically get GetNativeSystemInfo
    pfnGetNativeSystemInfo =
        reinterpret_cast<GetNativeSystemInfo_t>(GetProcAddress(hKernel32, "GetNativeSystemInfo"));
    // Windows 2000 won't have this function, use GetSystemInfo instead
    if (pfnGetNativeSystemInfo) {
      whichfunc = L"pfnGetNativeSystemInfo";
      pfnGetNativeSystemInfo(&sysInfo);
    } else {
      whichfunc = L"GetSystemInfo";
      GetSystemInfo(&sysInfo); // Directly run GetSystemInfo, even NT 4 has this.
    }
  }
  if (is_debug) {
    LOG(DEBUG) << L"Using " << whichfunc << " for " << __FUNC__;
  }
  const DWORD num_cpus = sysInfo.dwNumberOfProcessors;
  g_num_cpus           = static_cast<int>(num_cpus);
  return num_cpus;
}

// Samples per-process and (where supported) system-wide CPU times, computes
// the % CPU this app used since the previous sample, updates g_snapshot,
// and returns g_snapshot.cpu_percent clamped to [0.0, 100.0] - or NaN on
// the very first call, where there is no previous sample to delta against
// (callers use std::isnan to recognize "baseline only, nothing to display
// yet" and leave the status bar's initial placeholder in place).
//
// Why GetProcessTimes (not NtQuerySystemInformation): GetProcessTimes is
// fully documented, NT 3.5+, and aggregates over ALL threads in the process
// automatically - so the entire ant pool is captured in one call. NtQuery's
// SYSTEM_PROCESS_INFORMATION would give the same numbers but requires
// walking a variable-layout linked list of every process to find ourselves.
//
// Normalization: dProcCpu / (dWall * g_num_cpus). On a 4-core machine an
// app pinning one core reports 25%, matching Task Manager's "% CPU" column.
float cpu::GetCPUPercent() {
  // Sample everything up front so the timestamps are as close together as
  // possible; otherwise the deltas drift relative to one another.
  FILETIME ftWall;
  GetSystemTimeAsFileTime(&ftWall);

  FILETIME ftCreate, ftExit, ftProcKernel, ftProcUser;
  if (!GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftProcKernel, &ftProcUser)) {
    LOG(ERROR) << L"GetProcessTimes failed (last error " << logging::Hex(GetLastError()) << L")";
    return 0.0f;
  }

  FILETIME ftSysIdle = {}, ftSysKernel = {}, ftSysUser = {};
  bool haveSysTimes = false;
  if (pfnGetSystemTimes != nullptr) {
    haveSysTimes = (pfnGetSystemTimes(&ftSysIdle, &ftSysKernel, &ftSysUser) != FALSE);
  }

  // First call: resolve GetSystemTimes once, stash baseline, return 0. The
  // next tick is where a meaningful delta becomes available.
  if (!s_haveLastSample) {
    if (!hKernel32) {
      LOG(ERROR) << L"Failed to get kernel32.dll!";
      return std::numeric_limits<float>::quiet_NaN();
    } else {
      pfnGetSystemTimes =
          reinterpret_cast<GetSystemTimes_t>(GetProcAddress(hKernel32, "GetSystemTimes"));
    }
    if (pfnGetSystemTimes == nullptr) {
      LOG(WARN) << L"GetSystemTimes unavailable - system idle % will report 0 (Windows 2000?).";
    } else {
      // Resolved this call - sample once so the next tick has a baseline.
      pfnGetSystemTimes(&ftSysIdle, &ftSysKernel, &ftSysUser);
      s_lastSysIdle   = ftSysIdle;
      s_lastSysKernel = ftSysKernel;
      s_lastSysUser   = ftSysUser;
    }
    s_lastWall       = ftWall;
    s_lastProcKernel = ftProcKernel;
    s_lastProcUser   = ftProcUser;
    s_haveLastSample = true;
    // NaN signals "baseline only, no meaningful reading yet". UpdateCpuUsage
    // checks std::isnan and leaves the status bar's initial placeholder
    // ("CPU Usage: NaN" set in InitStatusBar) untouched until the next tick
    // gives us a real delta.
    return std::numeric_limits<float>::quiet_NaN();
  }

  // Process deltas (100-ns units, all unsigned).
  const ULONGLONG dWall   = FileTimeToU64(ftWall) - FileTimeToU64(s_lastWall);
  const ULONGLONG dKernel = FileTimeToU64(ftProcKernel) - FileTimeToU64(s_lastProcKernel);
  const ULONGLONG dUser   = FileTimeToU64(ftProcUser) - FileTimeToU64(s_lastProcUser);
  const ULONGLONG dCpu    = dKernel + dUser;
  s_lastWall              = ftWall;
  s_lastProcKernel        = ftProcKernel;
  s_lastProcUser          = ftProcUser;

  float cpuPct    = 0.0f;
  float kernelPct = 0.0f;
  if (dWall > 0 && g_num_cpus > 0) {
    // Use double for the divide so very small percentages don't lose precision.
    const double denom = static_cast<double>(dWall) * static_cast<double>(g_num_cpus);
    cpuPct             = static_cast<float>((static_cast<double>(dCpu) * 100.0) / denom);
    kernelPct          = static_cast<float>((static_cast<double>(dKernel) * 100.0) / denom);
  }
  // Clamp - rounding plus the kernel only updating GetProcessTimes
  // periodically can land just outside [0, 100] on a fast tick.
  if (cpuPct < 0.0f) {
    cpuPct = 0.0f;
  }
  if (cpuPct > 100.0f) {
    cpuPct = 100.0f;
  }
  if (kernelPct < 0.0f) {
    kernelPct = 0.0f;
  }
  if (kernelPct > 100.0f) {
    kernelPct = 100.0f;
  }

  float idlePct = 0.0f;
  if (haveSysTimes) {
    // GetSystemTimes' kernel time INCLUDES idle, so total = kernel + user
    // and idle is a fraction of that total (no need to multiply by num_cpus
    // - GetSystemTimes already aggregates across all CPUs).
    const ULONGLONG dSysIdle = FileTimeToU64(ftSysIdle) - FileTimeToU64(s_lastSysIdle);
    const ULONGLONG dSysKern = FileTimeToU64(ftSysKernel) - FileTimeToU64(s_lastSysKernel);
    const ULONGLONG dSysUser = FileTimeToU64(ftSysUser) - FileTimeToU64(s_lastSysUser);
    const ULONGLONG dSysTot  = dSysKern + dSysUser;
    if (dSysTot > 0) {
      idlePct = static_cast<float>((static_cast<double>(dSysIdle) * 100.0) /
                                   static_cast<double>(dSysTot));
    }
    if (idlePct < 0.0f) {
      idlePct = 0.0f;
    }
    if (idlePct > 100.0f) {
      idlePct = 100.0f;
    }
    s_lastSysIdle   = ftSysIdle;
    s_lastSysKernel = ftSysKernel;
    s_lastSysUser   = ftSysUser;
  }

  g_snapshot.cpu_percent    = cpuPct;
  g_snapshot.kernel_percent = kernelPct;
  g_snapshot.idle_percent   = idlePct;

  return cpuPct;
}

// Get this app's CPU percent usage, and display it in status bar, every TIMER_CPU tick
void UpdateCpuUsage() {
  const float kCpuUsage = cpu::GetCPUPercent();

  // GetCPUPercent returns NaN on its first call - it has only a baseline
  // sample, no delta yet. Skip the bar update so the user keeps seeing the
  // initial "CPU Usage: NaN" placeholder until the next tick produces a
  // real reading; checked BEFORE the bounds check below because NaN
  // compares false against everything and would otherwise look out-of-bounds.
  if (std::isnan(kCpuUsage)) {
    return;
  }

  // Sanity-check the percent before formatting it. A value outside [0, 100]
  // means GetCPUPercent miscomputed somewhere - log and trip the DCHECK so
  // it surfaces in dev builds, then bail (don't push garbage to the bar).
  const bool valid_percent = (kCpuUsage >= 0.0f && kCpuUsage <= 100.0f);
  if (!valid_percent) {
    LOG(ERROR) << L"CPU % out of bounds: " << kCpuUsage;
    DCHECK(valid_percent);
    return;
  }

  if (hStatusBar == nullptr) {
    // One-shot log: TIMER_CPU fires every g_perf_delay ms; we don't want the
    // log file growing forever just because the status bar failed to init.
    static bool s_logged_null_bar = false;
    if (!s_logged_null_bar) {
      LOG(ERROR) << L"hStatusBar is null - CPU usage will not be displayed.";
      s_logged_null_bar = true;
    }
    return;
  }
  // %.1f truncates to one decimal place. swprintf is the C++17-compatible
  // route to formatted floats; std::format would do this in one line but
  // requires C++20 + libstdc++ 13+, which the legacy-Windows build can't
  // assume. 16 wchars is comfortably more than "100.0%\0" needs.
  wchar_t buf[16];
  swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f%%", kCpuUsage);
  const std::wstring kCpuBubbleText = std::wstring(L"CPU Usage: ") + buf;
  UpdateStatusBar(1, kCpuBubbleText);
}

// Simply return g_snapshot
const PerfSnapshot& GetPerfSnapshot() {
  return g_snapshot;
}

void InitCpuMon(HWND hWnd) {
  if (hWnd == nullptr) {
    return;
  } else {
    // Note: Do not call FreeLibrary() on GetModuleHandle() results
    hKernel32 = GetModuleHandleW(L"kernel32.dll");
  }
  // Start CPU monitoring right before starting any ants
  const DWORD kNumCpus = cpu::GetLogicalProcessorCount();
  // Update once, then after every TIMER_CPU timer tick below.
  UpdateCpuUsage();
  // Set CPU performance monitoring timer
  SetTimer(hWnd, TIMER_CPU, g_perf_delay, nullptr);
  LOG(DEBUG) << L"Started CPU usage monitoring of PID " << GetCurrentProcessId();
  LOG(DEBUG) << L"Num. Logical CPU Cores: " << kNumCpus;
}

void ShutDownCpuMon() {
  KillTimer(mainHwnd, TIMER_CPU); // Kill CPU perf timer
  pfnGetSystemTimes = nullptr;
}
