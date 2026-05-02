#ifndef LANGTON_ANTS_CPU_H_
#define LANGTON_ANTS_CPU_H_

// clang-format off
#include "framework.h"

#include <winternl.h>

// clang-format on

/* Typedefs for accessing system .dll functions through GetProcAddress() */
typedef void(WINAPI* GetNativeSystemInfo_t)(SYSTEM_INFO* lpSystemInfo);

// Number of logical CPU threads in system
extern int g_num_cpus;

// How long between timer ticks for CPU stockkeeping, in milliseconds
extern const unsigned int g_perf_delay;

struct PerfSnapshot {
  float cpu_percent;    // Total app CPU utilization % (user + kernel, excluding idle)
  float kernel_percent; // Kernel-only app CPU utilization % (excluding idle)
  float idle_percent;   // System-wide idle CPU % (whole machine, NOT just this app);
                        // useful as a sanity-check denominator against cpu_percent.
};

extern PerfSnapshot g_snapshot;

// These two functions are internal, keep them in their own namespace
namespace cpu {
  // Gets the number of logical CPU threads of the host system.
  DWORD GetLogicalProcessorCount();

  // Gets the total CPU usage percent of this app, including all it's threads
  // taking into account SMP. Updates CPU perf snapshot.
  float GetCPUPercent();
} // namespace

// This is called once per TIMER_CPU tick to run GetCPUPercent() and
// update the status bar CPU status bubble.
void UpdateCpuUsage();

// Returns a const reference to the last snapshot produced by UpdateCPUPerfData().
const PerfSnapshot& GetPerfSnapshot();

// Initialize CPU monitoring. Call before first UpdateCpuUsage() call.
void InitCpuMon(HWND hWnd);

// Kills CPU monitoring timer, cleans up any handles.
void ShutDownCpuMon();

#endif // LANGTON_ANTS_CPU_H_
