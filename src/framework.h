#ifndef LANGTON_ANTS_FRAMEWORK_H_
#define LANGTON_ANTS_FRAMEWORK_H_

// clang-format off
#include "version.h" // Keep this at the top!

#define NOMINMAX
#include <windows.h>  // Main Windows header
#include <commctrl.h> // Common Controls
#include <commdlg.h>  // Common dialogs
#include <tchar.h>    // Unicode TCHAR

#include <algorithm> // std::min / std::max
#include <iostream>  // Console output and ostringstream
#include <random>    // Randomization functions
#include <string>    // std::string / std::wstring
#include <vector>    // Storage, used for Pixel buffers, etc.

#ifdef __cplusplus
 #if __cplusplus < 201103L || !defined(__cplusplus)
  // For old compilers without constexpr or inline
  #if !defined(constexpr) || !defined(__cpp_constexpr)
   typedef const constexpr;
  #endif // constexpr
  #if !defined(inline)
   #define inline
  #endif // inline
 #endif
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
