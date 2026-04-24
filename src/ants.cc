#include "ants.h"

#include "globals.h"
#include "resource.h"
#include "utils.h"

volatile bool g_running = false; // Global ant threads running state
volatile bool g_paused  = false; // Affects g_running, used by IDM_PAUSED

bool g_monochrome = false; // Whether monochrome colors only is enabled

volatile UINT g_num_ants = 1; // Initialize to 1, in case something goes wrong at least we draw 1 ant

unsigned long g_delay = 500UL; // Default to same as .rc file (IDM_FAST)

// --- Thread pool state ----------------------------------------------------
// Each live ant thread has its own auto-reset "tick" event and an exit flag.
// WM_TIMER (via SignalAntsTick) calls SetEvent on exactly s_activeCount of
// these every tick, so each thread wakes once per tick and moves the ant by one space.
// This keeps total ants-per-tick == thread count (== g_num_ants), and
// lets us dynamically spawn/terminate individual threads when the user
// changes the Num Ants setting.
struct AntThreadSlot {
  HANDLE        hThread       = nullptr;
  HANDLE        hTickEvent    = nullptr; // auto-reset; SetEvent = "go draw"
  volatile bool exitRequested = false;   // set true to make thread exit cleanly
};
static AntThreadSlot s_slots[kMaxAntThreads];
static int           s_activeCount = 0;  // only touched from the main thread

// Path/ant color chosen for contrast against the current background: white
// unless the background is white or green, in which case black. Re-read on
// every step so the ant adapts immediately if the user changes the bg
// mid-simulation. (Existing trails are left alone — see the comment on
// RecolorBackground.)
static COLORREF CurrentPathColor() {
  if (g_bkg_color == RGB_WHITE || g_bkg_color == RGB_GREEN) return RGB_BLACK;
  return RGB_WHITE;
}

DWORD WINAPI AntThread(LPVOID pvoid) {
  AntThreadSlot* slot = static_cast<AntThreadSlot*>(pvoid);
  if (mainHwnd == nullptr || slot == nullptr) {
    return 0x00000001;
  }
  // Per-ant state, thread-local so no synchronization is needed for it. The
  // shared state (g_hdcMem / the back buffer bitmap) is protected by
  // g_paintCS inside the tick loop.
  //   cellX / cellY — current cell in the CELL_PX grid, negative means
  //                   "needs placement" (first tick, or canvas shrank under
  //                   us and our old cell is out of range).
  //   dir           — 0=N, 1=E, 2=S, 3=W. Right turn = +1, left = +3,
  //                   reverse = +2, all mod 4.
  // rand() on Win32 uses per-thread state, so srand'ing here seeds only
  // this thread's sequence. Mixing GetTickCount() with the thread ID keeps
  // simultaneous starts distinct — otherwise the eight ants would spawn at
  // the same cell facing the same way.
  srand(static_cast<unsigned>(GetTickCount() ^ GetCurrentThreadId()));
  int cellX = -1, cellY = -1;
  int dir   = 0;
  // Direction → (dx, dy) in cell units, matching the encoding above.
  static const int kDx[4] = {  0, 1, 0, -1 };
  static const int kDy[4] = { -1, 0, 1,  0 };

  while (true) {
    // Block until SignalAntsTick signals this slot's private event. Auto-reset,
    // so it returns to non-signalled immediately and we block again on the
    // next iteration. Any failure / spurious wake exits the thread.
    if (slot->hTickEvent == nullptr ||
        WaitForSingleObject(slot->hTickEvent, INFINITE) != WAIT_OBJECT_0) {
      break;
    }
    // Two exit paths: global shutdown OR this individual slot was asked to die
    // (EnsureThreadCount shrinking the pool).
    if (!g_running || slot->exitRequested) break;
    if (cxClient == 0 && cyClient == 0) {
      continue; // Window is minimized; wait for restore
    }

    // Serialize every GDI operation on the back buffer — multiple ant threads
    // can be inside this section trying to enter at the same time, and the
    // main thread also grabs it in WM_PAINT and RecreateBackBuffer.
    EnterCriticalSection(&g_paintCS);
    if (g_hdcMem != nullptr) {
      // Quantize the canvas into a CELL_PX × CELL_PX grid. Integer division
      // truncates any remainder column / row so ants never straddle the
      // right / bottom edge. We need at least a 2×2 grid to bounce within;
      // anything smaller, skip the tick.
      const int gridW = cxClient / CELL_PX;
      const int gridH = cyClient / CELL_PX;
      if (gridW >= 2 && gridH >= 2) {
        // First-tick placement, or recovery after a resize that shrank the
        // grid below our current cell. Uniform across the grid with a
        // random starting heading.
        if (cellX < 0 || cellY < 0 || cellX >= gridW || cellY >= gridH) {
          cellX = rand() % gridW;
          cellY = rand() % gridH;
          dir   = rand() % 4;
        }

        const int px = cellX * CELL_PX;
        const int py = cellY * CELL_PX;
        // Every cell is painted as a solid CELL_PX × CELL_PX block, so
        // sampling any one of its pixels tells us the whole cell's logical
        // color. Top-left is convenient.
        const COLORREF sampled = GetPixel(g_hdcMem, px, py);
        // Binary classification: anything matching the current bg counts
        // as "unvisited", anything else is "path". That way stale trails
        // left over from before a bg change still read as path and fold
        // back into the automaton's state correctly.
        const bool onBg = (sampled == g_bkg_color);
        // Classic Langton's: on bg cell turn right, on path cell turn left,
        // flip the cell's color, then step forward one cell.
        dir = onBg ? (dir + 1) & 3 : (dir + 3) & 3;
        const COLORREF paintColor = onBg ? CurrentPathColor() : g_bkg_color;
        RECT rc = { px, py, px + CELL_PX, py + CELL_PX };
        HBRUSH hBrush = CreateSolidBrush(paintColor);
        FillRect(g_hdcMem, &rc, hBrush);
        DeleteObject(hBrush);

        // Try to step forward. If that would leave the grid, reverse 180°
        // and step the other way — "bouncing off the wall". We don't undo
        // the flip we just did; only the direction changes.
        int nx = cellX + kDx[dir];
        int ny = cellY + kDy[dir];
        if (nx < 0 || nx >= gridW || ny < 0 || ny >= gridH) {
          dir = (dir + 2) & 3;
          nx = cellX + kDx[dir];
          ny = cellY + kDy[dir];
        }
        cellX = nx;
        cellY = ny;

        // Invalidate just the cell we painted so WM_PAINT blits a tight
        // region instead of the whole canvas. InvalidateRect is documented
        // as safe to call from any thread; it just posts WM_PAINT to the
        // window's owning (main) thread. Coords need shifting by
        // g_toolbarHeight to go from back-buffer space into window-client
        // space.
        RECT inval = { px, py + g_toolbarHeight,
                       px + CELL_PX, py + CELL_PX + g_toolbarHeight };
        InvalidateRect(mainHwnd, &inval, FALSE);
      }
    }
    LeaveCriticalSection(&g_paintCS);
    // GdiFlush ensures all batched GDI operations for this thread are submitted
    // to the driver. Required on Windows 10/11 where DWM batches more aggressively
    // and ant paths may otherwise not appear until the batch is flushed naturally.
    GdiFlush();
  }
  return 0x00000000;
}

// --- Thread pool management -----------------------------------------------
// These run on the main (UI) thread, never from inside AntThread itself, so
// mutating s_slots / s_activeCount doesn't need its own critical section.

bool EnsureThreadCount(int targetCount) {
  if (targetCount < 1)              targetCount = 1;
  if (targetCount > kMaxAntThreads) targetCount = kMaxAntThreads;

  // Grow: spawn new slots up to targetCount.
  while (s_activeCount < targetCount) {
    const int i = s_activeCount;
    s_slots[i].exitRequested = false;
    s_slots[i].hTickEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s_slots[i].hTickEvent == nullptr) return false;
    s_slots[i].hThread = CreateThread(nullptr, 0, AntThread, &s_slots[i], 0, nullptr);
    if (s_slots[i].hThread == nullptr) {
      CloseHandle(s_slots[i].hTickEvent);
      s_slots[i].hTickEvent = nullptr;
      return false;
    }
    s_activeCount++;
  }

  // Shrink: ask the highest-indexed threads to exit, one by one. The thread
  // can only observe exitRequested after a wake, so we SetEvent to force it
  // to run the check. Then join and clean up.
  while (s_activeCount > targetCount) {
    const int i = s_activeCount - 1;
    s_slots[i].exitRequested = true;
    SetEvent(s_slots[i].hTickEvent);
    WaitForSingleObject(s_slots[i].hThread, INFINITE);
    CloseHandle(s_slots[i].hThread);
    CloseHandle(s_slots[i].hTickEvent);
    s_slots[i].hThread    = nullptr;
    s_slots[i].hTickEvent = nullptr;
    s_activeCount--;
  }
  return true;
}

void SignalAntsTick() {
  // Release one tick to each currently-active thread. Auto-reset events mean
  // each SetEvent wakes exactly one waiter (the thread waiting on that specific
  // event), so all s_activeCount threads wake together per tick.
  for (int i = 0; i < s_activeCount; i++) {
    if (s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
}

void ShutdownAnts() {
  g_running = false;
  // Wake every live thread so they can observe g_running=false and exit.
  for (int i = 0; i < s_activeCount; i++) {
    if (s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
  // Then join + close handles.
  for (int i = 0; i < s_activeCount; i++) {
    if (s_slots[i].hThread != nullptr) {
      WaitForSingleObject(s_slots[i].hThread, INFINITE);
      CloseHandle(s_slots[i].hThread);
      s_slots[i].hThread = nullptr;
    }
    if (s_slots[i].hTickEvent != nullptr) {
      CloseHandle(s_slots[i].hTickEvent);
      s_slots[i].hTickEvent = nullptr;
    }
  }
  s_activeCount = 0;
}

// Creates or replaces the off-screen back buffer to match the current client
// area size. A "back buffer" is an off-screen bitmap we draw into before
// presenting to the screen. This lets WM_PAINT restore any region that gets
// invalidated (e.g. another window dragged over ours) without losing ant paths.
//
// A "compatible" DC/bitmap mirrors the pixel format of the real window DC so
// that BitBlt can copy between them without color conversion overhead.
void RecreateBackBuffer(HWND hWnd, int cx, int cy) {
  if (cx <= 0 || cy <= 0 || g_hdcMem == nullptr) return;
  // Borrow the window DC only to query its pixel format for CreateCompatibleBitmap.
  HDC hdcWin = GetDC(hWnd);
  HBITMAP hbmNew = CreateCompatibleBitmap(hdcWin, cx, cy);
  ReleaseDC(hWnd, hdcWin);
  // Hold the lock while swapping the bitmap so the ant thread cannot draw into
  // g_hdcMem while we are replacing what it points at.
  EnterCriticalSection(&g_paintCS);
  // SelectObject swaps the new bitmap into the memory DC, making g_hdcMem ready
  // to draw into at the new size. The previously selected bitmap is implicitly
  // deselected and safe to delete.
  SelectObject(g_hdcMem, hbmNew);
  if (g_hbmMem != nullptr) DeleteObject(g_hbmMem);
  g_hbmMem = hbmNew;
  // Fill the fresh bitmap with the current background color so newly exposed
  // areas on resize match the rest of the canvas.
  RECT rc = { 0, 0, cx, cy };
  HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
  FillRect(g_hdcMem, &rc, hBrush);
  DeleteObject(hBrush);
  LeaveCriticalSection(&g_paintCS);
}

// Rewrites every pixel in the back buffer that currently equals oldColor so
// it becomes newColor. Ant and ant path pixels are left alone because their RGB values
// don't match the old background. Uses GetDIBits/SetDIBits to pull the bitmap
// into a CPU buffer, swap pixels in a tight loop, then push back.
//
// COLORREF is stored as 0x00BBGGRR (little-endian DWORD). A 32-bit BI_RGB DIB
// stores each pixel as BGRA in memory, which reads as 0xAARRGGBB as a DWORD.
// R and B are swapped between the two representations, so we build the
// comparison/replacement DWORDs explicitly rather than comparing COLORREFs.
void RecolorBackground(COLORREF oldColor, COLORREF newColor) {
  if (oldColor == newColor) return;

  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem == nullptr || g_hbmMem == nullptr ||
      cxClient <= 0 || cyClient <= 0) {
    LeaveCriticalSection(&g_paintCS);
    return;
  }

  const int width  = cxClient;
  const int height = cyClient;

  BITMAPINFOHEADER bi = {};
  bi.biSize        = sizeof(BITMAPINFOHEADER);
  bi.biWidth       = width;
  bi.biHeight      = -height; // negative = top-down (simpler indexing)
  bi.biPlanes      = 1;
  bi.biBitCount    = 32;
  bi.biCompression = BI_RGB;

  std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
  GetDIBits(g_hdcMem, g_hbmMem, 0, height, pixels.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

  // Convert the two COLORREFs to the DIB's DWORD representation.
  const DWORD oldPix = (GetRValue(oldColor) << 16) |
                       (GetGValue(oldColor) << 8)  |
                        GetBValue(oldColor);
  const DWORD newPix = (GetRValue(newColor) << 16) |
                       (GetGValue(newColor) << 8)  |
                        GetBValue(newColor);

  // Mask off the high (reserved/alpha) byte when comparing so any noise there
  // doesn't cause false negatives on pixels that should match.
  for (auto& p : pixels) {
    if ((p & 0x00FFFFFF) == oldPix) {
      p = (p & 0xFF000000) | newPix;
    }
  }

  SetDIBits(g_hdcMem, g_hbmMem, 0, height, pixels.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

  LeaveCriticalSection(&g_paintCS);
}

void SetNumAnts(const unsigned int num) {
  unsigned int clamped = num;
  if (clamped > kMaxAntThreads) clamped = kMaxAntThreads;
  if (clamped == 0)             clamped = 1;
  g_num_ants = clamped;
  // If the pool is already running (i.e. we're past ShowAnts), resize it to
  // match. Before ShowAnts there is nothing to resize — ShowAnts will spawn
  // the right number of threads using g_num_ants directly.
  if (g_running) {
    EnsureThreadCount(static_cast<int>(clamped));
  }
}

bool ShowAnts() {
  if (g_num_ants == 0 || g_delay == 0) {
    LOG(ERROR) << L"Number of ants or delay Out of bounds!";
    return false;
  }

  // Spin up the initial thread pool matching the current Num Ants
  // setting. Each thread owns its own auto-reset wake event and blocks on
  // it until SignalAntsTick (driven by WM_TIMER) says "go."
  g_running = true;
  if (!EnsureThreadCount(static_cast<int>(g_num_ants))) {
    ShutdownAnts();
    return false;
  }

  // Start the timer that drives drawing. WM_TIMER fires every g_delay ms
  // and, via SignalAntsTick, pulses every active thread's tick event once.
  if (!SetTimer(mainHwnd, TIMER_ANTS, g_delay, nullptr)) {
    ShutdownAnts();
    return false;
  }
  return true;
}

void TogglePaintAnts(HWND hWnd) {
  if (hWnd == nullptr) {
    return;
  }
  g_paused = !g_paused;
  // Pause = kill the timer so no more ticks fire. Every active thread sits
  // parked on its tick event, zero CPU. Resume = re-arm the timer and also
  // give one immediate pulse so the window doesn't wait up to g_delay ms
  // before redrawing.
  if (g_paused) {
    KillTimer(hWnd, TIMER_ANTS);
  } else {
    SignalAntsTick();
    SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr);
  }
}
