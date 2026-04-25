#include "ants.h"

#include "globals.h"
#include "resource.h"
#include "sound.h"
#include "utils.h"

volatile bool g_running = false; // Global ant threads running state
volatile bool g_paused  = false; // Affects g_running, used by IDM_PAUSED

bool g_monochrome = false; // Whether monochrome colors only is enabled

volatile UINT g_num_ants = 1; // Initialize to 1, in case something goes wrong at least we draw 1 ant

unsigned long g_delay = kHyperSpeed; // Default until InitMenuDefaults reads the RC.

// --- Thread pool state ----------------------------------------------------
// Each live ant thread has its own auto-reset "tick" event and an exit flag.
// WM_TIMER (via SignalAntsTick) calls SetEvent on exactly s_activeCount of
// these every tick, so each thread wakes once per tick and moves the ant by one space.
// This keeps total ants-per-tick == thread count (== g_num_ants), and
// lets us dynamically spawn/terminate individual threads when the user
// changes the Num Ants setting.
struct AntThreadSlot {
  HANDLE        hThread          = nullptr;
  HANDLE        hTickEvent       = nullptr; // auto-reset; SetEvent = "go draw"
  volatile bool exitRequested    = false;   // set true to make thread exit cleanly
  volatile bool reseedRequested  = false;   // set true to reroll position / color / dir
};
static AntThreadSlot s_slots[kMaxAntThreads];
static int           s_activeCount = 0;  // only touched from the main thread

// Path/ant color chosen for contrast against the current background: white
// unless the background is white or green, in which case black. Re-read on
// every step so the ant adapts immediately if the user changes the bg
// mid-simulation. (Existing trails are left alone — see the comment on
// RecolorBackground.)
static COLORREF CurrentPathColor() {
  if (g_bkg_color == RGB_WHITE || g_bkg_color == RGB_GREEN) {
    return RGB_BLACK;
  }
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
  // Per-ant marker color. Picked once at placement (see needsPlacement
  // branch below) from {magenta, cyan, yellow} so multiple ants on the
  // canvas are easy to tell apart. Collision detection treats any of
  // those three as "another ant" — see isBlocked in the step branch.
  COLORREF antColor = RGB_MAGENTA;
  // onBg tracks whether the cell the ant is sitting on was an unvisited
  // background cell or a path cell *before* we painted the ant marker
  // over it. Stored semantically (bool) not as a raw COLORREF so it
  // still interprets correctly if g_bkg_color changes mid-flight.
  bool onBg = true;
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
    // Main thread may have requested a reseed (IDM_REPAINT). Clearing
    // cellX triggers the needsPlacement branch below, which rerolls
    // position, direction, and marker color from the current rand()
    // state — so each reseed produces a fresh layout.
    if (slot->reseedRequested) {
      slot->reseedRequested = false;
      cellX = -1;
    }
    if (cxClient == 0 || cyClient == 0) {
      continue; // Window is minimized or has no drawable canvas; wait.
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
        const bool needsPlacement = (cellX < 0 || cellY < 0 ||
                                     cellX >= gridW || cellY >= gridH);
        if (needsPlacement) {
          // First-tick placement, or recovery after a resize that shrank
          // the grid below our old cell. Sample the cell to decide onBg
          // (could be bg or a stale trail), roll this ant's marker color,
          // then overpaint. No Langton step this tick — next tick starts
          // stepping normally.
          cellX = rand() % gridW;
          cellY = rand() % gridH;
          dir   = rand() % 4;
          static const COLORREF kAntColors[3] = {
            RGB_MAGENTA, RGB_CYAN, RGB_YELLOW,
          };
          antColor = kAntColors[rand() % 3];
          const int px = cellX * CELL_PX;
          const int py = cellY * CELL_PX;
          const COLORREF sampled = GetPixel(g_hdcMem, px, py);
          onBg = (sampled == g_bkg_color);
          RECT antRc = { px, py, px + CELL_PX, py + CELL_PX };
          HBRUSH hAnt = CreateSolidBrush(antColor);
          FillRect(g_hdcMem, &antRc, hAnt);
          DeleteObject(hAnt);
          RECT inval = { px, py + g_toolbarHeight,
                         px + CELL_PX, py + CELL_PX + g_toolbarHeight };
          InvalidateRect(mainHwnd, &inval, FALSE);
        } else {
          // Classic Langton's step. We can't GetPixel the cell under the
          // ant — it's magenta — so we use the cached onBg from when the
          // ant arrived. On bg cell turn right, on path cell turn left,
          // flip the cell's color, then step forward one cell.
          dir = onBg ? (dir + 1) & 3 : (dir + 3) & 3;
          const COLORREF trailColor = onBg ? CurrentPathColor() : g_bkg_color;
          const int px = cellX * CELL_PX;
          const int py = cellY * CELL_PX;
          // Overpaint the vacating cell with the flipped trail color.
          // This both performs the Langton flip and removes the magenta
          // overlay, leaving a clean mark the next ant will classify
          // correctly via GetPixel.
          RECT trailRc = { px, py, px + CELL_PX, py + CELL_PX };
          HBRUSH hTrail = CreateSolidBrush(trailColor);
          FillRect(g_hdcMem, &trailRc, hTrail);
          DeleteObject(hTrail);

          // Try to step forward. A target cell is "blocked" if it's out of
          // bounds (wall) or currently occupied by another ant (magenta).
          // On block, reverse direction 180° and try the other way — the
          // same "bounce" rule covers both walls and ant-vs-ant collisions.
          // If the reversed cell is also blocked, stay put for this tick;
          // the subsequent sample-at-new-cell step still works because it
          // reads the trail color we just painted on the vacating cell.
          auto isBlocked = [&](int x, int y) -> bool {
            if (x < 0 || x >= gridW || y < 0 || y >= gridH) return true;
            // Treat any of the three ant marker colors as "occupied by
            // another ant" — this ant might be magenta, the neighbor
            // might be cyan, etc. A trail pixel is always black/white,
            // so the false-positive surface here is small.
            const COLORREF c = GetPixel(g_hdcMem, x * CELL_PX, y * CELL_PX);
            return c == RGB_MAGENTA || c == RGB_CYAN || c == RGB_YELLOW;
          };
          int nx = cellX + kDx[dir];
          int ny = cellY + kDy[dir];
          if (isBlocked(nx, ny)) {
            dir = (dir + 2) & 3;
            nx = cellX + kDx[dir];
            ny = cellY + kDy[dir];
            if (isBlocked(nx, ny)) {
              nx = cellX;
              ny = cellY;
            }
          }
          cellX = nx;
          cellY = ny;

          // Sample the new cell to learn bg vs. path for the next tick's
          // turn decision. Anything matching the current bg counts as
          // "unvisited"; anything else (including stale trails from
          // before a bg change) counts as path.
          const int npx = cellX * CELL_PX;
          const int npy = cellY * CELL_PX;
          const COLORREF sampled = GetPixel(g_hdcMem, npx, npy);
          onBg = (sampled == g_bkg_color);

          // Paint the ant on the new cell using this ant's chosen marker
          // color (locked in at placement, see needsPlacement branch).
          RECT antRc = { npx, npy, npx + CELL_PX, npy + CELL_PX };
          HBRUSH hAnt = CreateSolidBrush(antColor);
          FillRect(g_hdcMem, &antRc, hAnt);
          DeleteObject(hAnt);

          // Invalidate both the trail cell and the new ant cell so
          // WM_PAINT blits both tight rects on the next paint pass.
          // InvalidateRect is documented as safe to call from any thread;
          // it just posts WM_PAINT to the window's owning (main) thread.
          // Coords shift by g_toolbarHeight to go from back-buffer space
          // into window-client space.
          RECT invalOld = { px, py + g_toolbarHeight,
                            px + CELL_PX, py + CELL_PX + g_toolbarHeight };
          RECT invalNew = { npx, npy + g_toolbarHeight,
                            npx + CELL_PX, npy + CELL_PX + g_toolbarHeight };
          InvalidateRect(mainHwnd, &invalOld, FALSE);
          InvalidateRect(mainHwnd, &invalNew, FALSE);
        }
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
    // Prefer scheduling ant threads over the BGM worker / other normal-
    // priority threads so short CPU blips (audio driver buffer reset on
    // clip loop, GDI contention, etc.) don't skip a tick and cause a
    // visible stutter on fast speed settings.
    SetThreadPriority(s_slots[i].hThread, THREAD_PRIORITY_ABOVE_NORMAL);
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

void ReseedAnts() {
  // Flag every active slot so the next tick re-rolls its cellX / cellY /
  // dir / antColor. Pulse the tick events so the reseed happens even
  // when paused — otherwise the ants would sit in their old positions
  // until the user unpaused, which defeats the "Repaint now" intent.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].reseedRequested = true;
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
  // Fast path: the existing bitmap already matches — keep it, no work,
  // no state loss. Common on restore-from-minimize without a resize.
  if (g_hbmMem != nullptr) {
    BITMAP bm = {};
    if (GetObjectW(g_hbmMem, sizeof(BITMAP), &bm) &&
        bm.bmWidth == cx && bm.bmHeight == cy) {
      return;
    }
  }
  // Slow path: dimensions changed, allocate a fresh bitmap. Borrow the
  // window DC only to query its pixel format for CreateCompatibleBitmap.
  HDC hdcWin = GetDC(hWnd);
  HBITMAP hbmNew = CreateCompatibleBitmap(hdcWin, cx, cy);
  ReleaseDC(hWnd, hdcWin);
  if (hbmNew == nullptr) return;

  // Hold the lock while swapping the bitmap so the ant thread cannot draw into
  // g_hdcMem while we are replacing what it points at.
  EnterCriticalSection(&g_paintCS);
  // Prime hbmNew through a scratch DC: fill with bg, then blit the old
  // back buffer's contents into the top-left. This preserves ant trails
  // across the resize — and also covers any minimize-then-restore path
  // where something fires an intermediate WM_SIZE and triggers this
  // slow branch. On grow, the extra margin stays bg; on shrink, the
  // excess rows / columns of the old bitmap get clipped off.
  HDC hdcScratch = CreateCompatibleDC(g_hdcMem);
  HBITMAP hbmScratchPrev = static_cast<HBITMAP>(SelectObject(hdcScratch, hbmNew));
  RECT rc = { 0, 0, cx, cy };
  HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
  FillRect(hdcScratch, &rc, hBrush);
  DeleteObject(hBrush);
  if (g_hbmMem != nullptr) {
    BITMAP bmOld = {};
    if (GetObjectW(g_hbmMem, sizeof(BITMAP), &bmOld)) {
      const int copyW = (bmOld.bmWidth  < cx) ? bmOld.bmWidth  : cx;
      const int copyH = (bmOld.bmHeight < cy) ? bmOld.bmHeight : cy;
      BitBlt(hdcScratch, 0, 0, copyW, copyH, g_hdcMem, 0, 0, SRCCOPY);
    }
  }
  // Un-select hbmNew from the scratch DC so we can re-select it into g_hdcMem
  // (a bitmap can only be selected into one DC at a time).
  SelectObject(hdcScratch, hbmScratchPrev);
  DeleteDC(hdcScratch);

  // Promote hbmNew to be the live back buffer. SelectObject implicitly
  // deselects the previously-selected bitmap, which then becomes safe
  // to delete.
  SelectObject(g_hdcMem, hbmNew);
  if (g_hbmMem != nullptr) DeleteObject(g_hbmMem);
  g_hbmMem = hbmNew;
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
  // before redrawing. The BGM follows the simulation in lockstep via
  // AntPauseBgm / AntResumeBgm — single-step lands here too (it enters
  // the paused branch once, then further single-steps no-op this
  // toggle), so the BGM stays paused until the user un-pauses.
  if (g_paused) {
    KillTimer(hWnd, TIMER_ANTS);
    AntPauseBgm();
  } else {
    AntResumeBgm();
    SignalAntsTick();
    SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr);
  }
}
