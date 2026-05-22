#ifndef LANGTON_ANTS_FRAMEWORK_H_
#define LANGTON_ANTS_FRAMEWORK_H_

// clang-format off
#include "version.h" // Keep this at the top!

#define NOMINMAX

// On MSVC, <windows.h> defines a small subset of NTSTATUS codes, which then
// conflict with the full set in <ntstatus.h>. WIN32_NO_STATUS suppresses those
// definitions so <ntstatus.h> can own them without redefinition errors.
// MinGW handles this automatically, so the guard is MSVC-only.
#ifdef _MSC_VER
 #define WIN32_NO_STATUS
#endif
#include <windows.h> // Main Windows header
#ifdef _MSC_VER
 #undef WIN32_NO_STATUS
#endif
#include <commctrl.h> // Common Controls
#include <commdlg.h>  // Common dialogs
#include <ntstatus.h> // Full NTSTATUS codes (e.g. STATUS_SUCCESS)

// C Runtime Headers
#include <tchar.h> // For TCHAR, and automatically deducing wchar_t type

// C++ STL Headers
#include <algorithm> // std::min / std::max
#include <iostream>  // Console output and ostringstream
#include <limits>    // Numeric limits
#include <random>    // Randomization functions
#include <string>    // std::string / std::wstring
#include <vector>    // Storage, used for Pixel buffers, etc.

#ifndef __cplusplus
 #error APP_NAME requires a C++ compiler
#endif
#if __cplusplus < 201103L
 // For old compilers without constexpr or inline
 #if !defined(constexpr) || !defined(__cpp_constexpr)
  #define constexpr const
 #endif // constexpr
 #if !defined(inline)
  #define inline
 #endif // inline
#endif

// clang-format on

// Alias
#ifndef __FUNC__
 #define __FUNC__ __func__
#endif

// Defines for missing windowsx.h definitions, don't want to inlcude
// the heavy .h file just for this one thing.
#if !defined(GET_X_LPARAM) || !defined(GET_Y_LPARAM)
 #define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
 #define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

// Convert compiler defines to usable bools
inline constexpr bool is_dcheck =
#ifdef DCHECK_ON
    true;
#else
    false;
#endif // DCHECK

inline constexpr bool is_debug =
#if defined(DEBUG) || defined(_DEBUG)
    true;
#else
    false;
#endif // defined(DEBUG) || defined(_DEBUG)

#endif // LANGTON_ANTS_FRAMEWORK_H_
