#include "ants.h"

#include "globals.h"
#include "resource.h"
#include "sound.h"
#include "utils.h"

volatile bool g_running = false; // Global ant threads running state
volatile bool g_paused  = false; // Affects g_running, used by IDM_PAUSED
volatile bool g_stopped = true;  // True at startup (no animation yet) and after IDM_STOP. Drives
                                 // "Play" vs "Resume" label on the pause/play toolbar button.

bool g_monochrome = false; // Whether monochrome colors only is enabled

COLORREF g_ant_color =
    kRandomAntColor; // Fixed ant marker color, or kRandomAntColor for per-ant random pick

volatile UINT g_num_ants =
    1; // Initialize to 1, in case something goes wrong at least we draw 1 ant

unsigned long g_delay = kRealTime; // Default until InitMenuDefaults reads the RC.

// --- Thread pool state ----------------------------------------------------
// Each live ant thread has its own auto-reset "tick" event and an exit flag.
// WM_TIMER (via SignalAntsTick) calls SetEvent on exactly s_activeCount of
// these every tick, so each thread wakes once per tick and moves the ant by one space.
// This keeps total ants-per-tick == thread count (== g_num_ants), and
// lets us dynamically spawn/terminate individual threads when the user
// changes the Num Ants setting.
struct AntThreadSlot {
  HANDLE hThread              = nullptr;
  HANDLE hTickEvent           = nullptr; // auto-reset; SetEvent = "go draw"
  volatile bool exitRequest   = false;   // set true to make thread exit cleanly
  volatile bool reseedRequest = false;   // set true to reroll position / color / dir
  // Place-mode handoff: when placementRequested is set the thread adopts
  // (placeCellX, placeCellY, placeColor, placeOnBg) on its next tick, picks
  // a random direction, and skips the step (the marker is already painted on
  // the canvas by PlaceAntAtClient). Cleared by the thread once consumed.
  volatile bool placementRequested = false;
  int placeCellX                   = 0;
  int placeCellY                   = 0;
  COLORREF placeColor              = 0;
  bool placeOnBg                   = true;
  volatile bool customSeedRequest  = false; // Whether to use custom seed for seeding randomization
  UINT customSeed =
      0; // When 0 or customSeedRequest = false, this is unused, otherwise use for srand()
  // Color-refresh handoff: when set, the thread re-picks antColor against
  // the current g_monochrome and overpaints its current cell so the new
  // color is visible immediately (even when paused). Position / dir /
  // onBg are left alone — used by the Monochrome toggle which is meant
  // to behave like picking a Colors entry (just swap colors, don't
  // touch ant draw state).
  volatile bool colorRefreshRequest = false;
};
static AntThreadSlot s_slots[kMaxAntThreads];
static int s_activeCount = 0; // only touched from the main thread

// --- Place-mode state -----------------------------------------------------
// All touched from the main (UI) thread only — set when the user enters
// place mode, populated by PlaceAntAtClient on each click, drained by
// ApplyPlacements when the user resumes. Per-entry onBg is sampled BEFORE
// we paint the ant marker so the thread that adopts this position knows
// whether it started on background (turn right) or a path (turn left).
struct PlacedAnt {
  int cellX;
  int cellY;
  COLORREF color;
  bool onBg;
};
static PlacedAnt s_placedAnts[kMaxAntThreads];
bool g_place_mode       = false;
int g_placed_ants_count = 0;

// Forward-declared so TogglePaintAnts can drain pending placements into
// thread slots before re-arming the timer. Returns true on success;
// returns false when the inner SetNumAnts/EnsureThreadCount couldn't
// resize the pool to match the placed-ant count, OR when called outside
// place mode (nothing to apply).
static bool ApplyPlacements();

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

DWORD WINAPI AntThread(LPVOID pvoid_in) {
  AntThreadSlot* slot = static_cast<AntThreadSlot*>(pvoid_in);
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
  // this thread's sequence. Mixing in something that varies per slot
  // keeps simultaneous starts distinct — otherwise all kMaxAntThreads
  // ants would spawn at the same cell facing the same way.
  //
  // Two seeding modes:
  //   - Custom seed (user typed one): mix the seed with the slot's
  //     index in s_slots, NOT the OS thread ID. Slot indices are 0..N-1
  //     and stable across runs, so the same custom seed + same ant
  //     count reproduces the same per-slot rand() sequence (and thus
  //     the same starting cells / directions / colors). The Fibonacci
  //     hash constant 0x9E3779B9 spreads tiny indices into well-
  //     distributed seeds so adjacent slots don't all spawn at near-
  //     identical positions.
  //   - No custom seed: mix GetTickCount() with the thread ID — the
  //     intent there is "different every run", so non-determinism is
  //     a feature, not a bug.
  DWORD seed;
  if (slot->customSeedRequest) {
    const DWORD slotIdx = static_cast<DWORD>(slot - s_slots);
    seed                = static_cast<DWORD>(slot->customSeed) ^ (slotIdx * 0x9E3779B9u);
  } else {
    seed = GetTickCount() ^ GetCurrentThreadId();
  }
  srand(static_cast<unsigned int>(seed));
  int cellX = -1, cellY = -1;
  int dir = 0;
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
  static const int kDx[4] = {0, 1, 0, -1};
  static const int kDy[4] = {-1, 0, 1, 0};

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
    if (!g_running || slot->exitRequest) {
      break;
    }
    // Main thread may have requested a reseed (IDM_REPAINT). Clearing
    // cellX triggers the needsPlacement branch below, which rerolls
    // position, direction, and marker color from the current rand()
    // state — so each reseed produces a fresh layout.
    if (slot->reseedRequest) {
      slot->reseedRequest = false;
      cellX               = -1;
    }
    // Place-mode handoff. The main thread painted the marker on the canvas
    // already and recorded what was under it (placeOnBg), so we adopt the
    // user-clicked position + the marker's color and skip stepping this
    // tick — the next tick will do a normal Langton step from here. The
    // direction is rolled from rand() (which may have been seeded by a
    // custom seed at thread startup), so a custom seed only varies the
    // direction in place mode; position and color stay user-controlled.
    if (slot->placementRequested) {
      slot->placementRequested = false;
      cellX                    = slot->placeCellX;
      cellY                    = slot->placeCellY;
      antColor                 = slot->placeColor;
      onBg                     = slot->placeOnBg;
      dir                      = rand() & 3;
      continue;
    }
    // Color-only refresh (Monochrome toggle). Re-pick antColor from the
    // current g_monochrome and overpaint the ant's current cell so the
    // new color shows up immediately (matters when paused — otherwise
    // the next Langton step would draw it anyway). Position, dir and
    // onBg are deliberately preserved — this mirrors how the background
    // colour menu only swaps pixels and never touches ant draw state.
    // cellX < 0 means we haven't placed yet; the next needsPlacement
    // branch will pick a color naturally, so we skip the paint.
    if (slot->colorRefreshRequest) {
      slot->colorRefreshRequest = false;
      if (g_monochrome) {
        antColor = CurrentPathColor();
      } else if (g_ant_color == kRandomAntColor) {
        static const COLORREF kAntColors[3] = {
            RGB_MAGENTA,
            RGB_CYAN,
            RGB_YELLOW,
        };
        antColor = kAntColors[rand() % 3];
      } else {
        antColor = g_ant_color;
      }
      if (cellX >= 0 && cellY >= 0) {
        EnterCriticalSection(&g_paintCS);
        if (g_hdcMem != nullptr) {
          const int px = cellX * CELL_PX;
          const int py = cellY * CELL_PX;
          RECT rc      = {px, py, px + CELL_PX, py + CELL_PX};
          HBRUSH hAnt  = CreateSolidBrush(antColor);
          FillRect(g_hdcMem, &rc, hAnt);
          DeleteObject(hAnt);
          RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
          InvalidateRect(mainHwnd, &inval, FALSE);
        }
        LeaveCriticalSection(&g_paintCS);
      }
      continue;
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
        const bool needsPlacement = (cellX < 0 || cellY < 0 || cellX >= gridW || cellY >= gridH);
        if (needsPlacement) {
          // First-tick placement, or recovery after a resize that shrank
          // the grid below our old cell. Sample the cell to decide onBg
          // (could be bg or a stale trail), roll this ant's marker color,
          // then overpaint. No Langton step this tick — next tick starts
          // stepping normally.
          cellX = rand() % gridW;
          cellY = rand() % gridH;
          dir   = rand() % 4;
          if (g_monochrome) {
            // Ant marker matches the trail color so the whole canvas is
            // pure black-on-grey or white-on-grey. The cell-level
            // distinction "this is an ant vs. this is a trail" goes away;
            // isBlocked below stops detecting ant-vs-ant collisions
            // (it keys off the magenta/cyan/yellow markers), so ants
            // simply pass through each other in monochrome mode.
            antColor = CurrentPathColor();
          } else if (g_ant_color == kRandomAntColor) {
            static const COLORREF kAntColors[3] = {
                RGB_MAGENTA,
                RGB_CYAN,
                RGB_YELLOW,
            };
            antColor = kAntColors[rand() % 3];
          } else {
            antColor = g_ant_color;
          }
          const int px           = cellX * CELL_PX;
          const int py           = cellY * CELL_PX;
          const COLORREF sampled = GetPixel(g_hdcMem, px, py);
          onBg                   = (sampled == g_bkg_color);
          RECT antRc             = {px, py, px + CELL_PX, py + CELL_PX};
          HBRUSH hAnt            = CreateSolidBrush(antColor);
          FillRect(g_hdcMem, &antRc, hAnt);
          DeleteObject(hAnt);
          RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
          InvalidateRect(mainHwnd, &inval, FALSE);
        } else {
          // Classic Langton's step. We can't GetPixel the cell under the
          // ant — it's magenta — so we use the cached onBg from when the
          // ant arrived. On bg cell turn right, on path cell turn left,
          // flip the cell's color, then step forward one cell.
          dir                       = onBg ? (dir + 1) & 3 : (dir + 3) & 3;
          const COLORREF trailColor = onBg ? CurrentPathColor() : g_bkg_color;
          const int px              = cellX * CELL_PX;
          const int py              = cellY * CELL_PX;
          // Overpaint the vacating cell with the flipped trail color.
          // This both performs the Langton flip and removes the magenta
          // overlay, leaving a clean mark the next ant will classify
          // correctly via GetPixel.
          RECT trailRc  = {px, py, px + CELL_PX, py + CELL_PX};
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
            if (x < 0 || x >= gridW || y < 0 || y >= gridH) {
              return true;
            }
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
            nx  = cellX + kDx[dir];
            ny  = cellY + kDy[dir];
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
          const int npx          = cellX * CELL_PX;
          const int npy          = cellY * CELL_PX;
          const COLORREF sampled = GetPixel(g_hdcMem, npx, npy);
          onBg                   = (sampled == g_bkg_color);

          // Paint the ant on the new cell using this ant's chosen marker
          // color (locked in at placement, see needsPlacement branch).
          RECT antRc  = {npx, npy, npx + CELL_PX, npy + CELL_PX};
          HBRUSH hAnt = CreateSolidBrush(antColor);
          FillRect(g_hdcMem, &antRc, hAnt);
          DeleteObject(hAnt);

          // Invalidate both the trail cell and the new ant cell so
          // WM_PAINT blits both tight rects on the next paint pass.
          // InvalidateRect is documented as safe to call from any thread;
          // it just posts WM_PAINT to the window's owning (main) thread.
          // Coords shift by g_toolbarHeight to go from back-buffer space
          // into window-client space.
          RECT invalOld = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
          RECT invalNew = {npx, npy + g_toolbarHeight, npx + CELL_PX,
                           npy + CELL_PX + g_toolbarHeight};
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
  if (targetCount < 1) {
    targetCount = 1;
  }
  if (targetCount > kMaxAntThreads) {
    targetCount = kMaxAntThreads;
  }

  // Grow: spawn new slots up to targetCount.
  while (s_activeCount < targetCount) {
    const int i            = s_activeCount;
    s_slots[i].exitRequest = false;
    s_slots[i].hTickEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s_slots[i].hTickEvent == nullptr) {
      return false;
    }
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
  // can only observe exitRequest after a wake, so we SetEvent to force it
  // to run the check. Then join and clean up.
  while (s_activeCount > targetCount) {
    const int i            = s_activeCount - 1;
    s_slots[i].exitRequest = true;
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

void RefreshAntColors() {
  // Flag every active slot for a color-only refresh, then pulse so the
  // change shows up immediately even while paused. The thread re-picks
  // antColor against the current g_monochrome and overpaints its current
  // cell — position / direction / onBg stay untouched, so the simulation
  // continues exactly where it was, just dressed in the new color scheme.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].colorRefreshRequest = true;
    if (s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
}

void ReseedAnts(bool pulse) {
  // Flag every active slot so the next tick re-rolls its cellX / cellY /
  // dir / antColor. When pulse is true, also SetEvent each tick event so
  // the reseed runs even while paused — that's the "Repaint now" path.
  // When pulse is false (IDM_STOP), the threads stay parked on their
  // tick events and the wiped canvas stays blank until something else
  // (typically the resume path in TogglePaintAnts) pulses them.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].reseedRequest     = true;
    s_slots[i].customSeedRequest = false;
    if (pulse && s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
}

bool CustomSeedAnts(const unsigned int custom_seed) {
  bool ok = true;
  // The custom seed is consumed inside AntThread's startup branch (srand),
  // never inside its tick loop, so applying a new seed means tearing down
  // every live ant thread and respawning the same number with the new seed
  // staged on their slots. The canvas is wiped so the new seed's output
  // starts from a clean state. Pause state is preserved: if the user was
  // playing, the timer is re-armed and the first tick fires immediately;
  // if the user was paused, the timer stays off and the new threads sit
  // on their tick events until the user presses play.
  if (s_activeCount <= 0) {
    LOG(ERROR) << L"No ant threads active, nothing to seed";
    return false;
  }
  const int desiredCount = s_activeCount;
  const bool wasRunning  = !g_paused;
  // Place mode and Custom Seed are not mutually exclusive: when both are
  // active, the user-clicked positions are kept and the seed only drives
  // the per-ant direction + color. Captured up front so the wipe-canvas
  // step below can be followed by a marker re-paint that preserves what
  // the user has already placed.
  const bool inPlaceMode = g_place_mode && g_placed_ants_count > 0;

  if (inPlaceMode) {
    LOG(INFO) << L"Using custom seed '" << custom_seed << L"' for ant direction";
  } else {
    LOG(INFO) << L"Using custom seed '" << custom_seed
              << L"' for ant placement, direction, and color.";
  }

  // Stop the timer for the duration of the respawn so no stray WM_TIMER
  // pulses arrive between teardown and the new threads being ready.
  if (mainHwnd != nullptr) {
    KillTimer(mainHwnd, TIMER_ANTS);
  }

  // Tear down every live thread. Mirrors the loop in ShutdownAnts but
  // leaves g_running = true so the freshly-spawned threads below don't
  // immediately exit on their first tick.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].exitRequest = true;
    if (s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
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

  // Wipe the back buffer to the current background color so the new seed's
  // layout starts from a clean canvas (matching the user's expectation that
  // changing the seed "repaints the whole thing").
  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
    RECT rc       = {0, 0, cxClient, cyClient};
    HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
    FillRect(g_hdcMem, &rc, hBrush);
    DeleteObject(hBrush);
    // Re-paint placement markers on the freshly-wiped canvas so the user's
    // clicks are still visible while paused. Their pre-seed color stays —
    // the AntThread placementRequested handler re-rolls antColor from the
    // seeded rand() once the simulation resumes, so the moving ant may
    // briefly differ in color until the first Langton step overpaints
    // the marker with the trail color.
    if (inPlaceMode && g_hdcMem != nullptr) {
      for (int i = 0; i < g_placed_ants_count; i++) {
        const PlacedAnt& a = s_placedAnts[i];
        const int px       = a.cellX * CELL_PX;
        const int py       = a.cellY * CELL_PX;
        RECT mrc           = {px, py, px + CELL_PX, py + CELL_PX};
        HBRUSH hAnt        = CreateSolidBrush(a.color);
        FillRect(g_hdcMem, &mrc, hAnt);
        DeleteObject(hAnt);
      }
    }
  }
  LeaveCriticalSection(&g_paintCS);

  // Stage the seed on the slot scratch fields BEFORE EnsureThreadCount
  // creates the threads — AntThread reads customSeedRequest in its
  // startup block, picks up customSeed, and srand's its per-thread rand()
  // from it. Reset the other request flags too so a stale placement /
  // reseed from a prior session can't fire on the very first tick.
  for (int i = 0; i < desiredCount; i++) {
    s_slots[i].customSeedRequest  = true;
    s_slots[i].customSeed         = custom_seed;
    s_slots[i].reseedRequest      = false;
    s_slots[i].placementRequested = false;
  }
  if (!EnsureThreadCount(desiredCount)) {
    LOG(ERROR) << L"EnsureThreadCount(" << desiredCount
               << L") failed during respawn — pool may be smaller than expected!";
    ok = false;
  }

  if (mainHwnd != nullptr) {
    InvalidateRect(mainHwnd, nullptr, FALSE);
  }

  // Restore the simulation's previous play state. If the user was running,
  // re-arm the timer and pulse so the first tick happens immediately
  // rather than waiting up to g_delay ms. If the user was paused, leave
  // the timer off — the threads sit on their tick events until resume.
  if (wasRunning && mainHwnd != nullptr) {
    SetTimer(mainHwnd, TIMER_ANTS, g_delay, nullptr);
    SignalAntsTick();
  }
  return ok;
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
bool RecreateBackBuffer(HWND hWnd, int cx, int cy) {
  bool ok = true;
  if (cx <= 0 || cy <= 0 || g_hdcMem == nullptr) {
    LOG(ERROR) << L"Invalid input (cx=" << cx << L", cy=" << cy << L", g_hdcMem="
               << (g_hdcMem ? L"set" : L"null") << L")";
    return false;
  }
  // Fast path: the existing bitmap already matches — keep it, no work,
  // no state loss. Common on restore-from-minimize without a resize.
  if (g_hbmMem != nullptr) {
    BITMAP bm = {};
    if (GetObjectW(g_hbmMem, sizeof(BITMAP), &bm) && bm.bmWidth == cx && bm.bmHeight == cy) {
      return true; // existing buffer is already the right size
    }
  }
  // Slow path: dimensions changed, allocate a fresh bitmap. Borrow the
  // window DC only to query its pixel format for CreateCompatibleBitmap.
  HDC hdcWin     = GetDC(hWnd);
  HBITMAP hbmNew = CreateCompatibleBitmap(hdcWin, cx, cy);
  ReleaseDC(hWnd, hdcWin);
  if (hbmNew == nullptr) {
    LOG(ERROR) << L"CreateCompatibleBitmap(" << cx << L"x" << cy
               << L") failed, out of GDI resources.";
    return false;
  }

  // Hold the lock while swapping the bitmap so the ant thread cannot draw into
  // g_hdcMem while we are replacing what it points at.
  EnterCriticalSection(&g_paintCS);
  // Prime hbmNew through a scratch DC: fill with bg, then blit the old
  // back buffer's contents into the top-left. This preserves ant trails
  // across the resize — and also covers any minimize-then-restore path
  // where something fires an intermediate WM_SIZE and triggers this
  // slow branch. On grow, the extra margin stays bg; on shrink, the
  // excess rows / columns of the old bitmap get clipped off.
  HDC hdcScratch         = CreateCompatibleDC(g_hdcMem);
  HBITMAP hbmScratchPrev = static_cast<HBITMAP>(SelectObject(hdcScratch, hbmNew));
  RECT rc                = {0, 0, cx, cy};
  HBRUSH hBrush          = CreateSolidBrush(g_bkg_color);
  FillRect(hdcScratch, &rc, hBrush);
  DeleteObject(hBrush);
  if (g_hbmMem != nullptr) {
    BITMAP bmOld = {};
    if (GetObjectW(g_hbmMem, sizeof(BITMAP), &bmOld)) {
      const int copyW = (bmOld.bmWidth < cx) ? bmOld.bmWidth : cx;
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
  if (g_hbmMem != nullptr) {
    DeleteObject(g_hbmMem);
  }
  g_hbmMem = hbmNew;
  LeaveCriticalSection(&g_paintCS);
  return ok;
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
  if (oldColor == newColor) {
    return;
  }

  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem == nullptr || g_hbmMem == nullptr || cxClient <= 0 || cyClient <= 0) {
    LeaveCriticalSection(&g_paintCS);
    return;
  }

  const int width  = cxClient;
  const int height = cyClient;

  BITMAPINFOHEADER bi = {};
  bi.biSize           = sizeof(BITMAPINFOHEADER);
  bi.biWidth          = width;
  bi.biHeight         = -height; // negative = top-down (simpler indexing)
  bi.biPlanes         = 1;
  bi.biBitCount       = 32;
  bi.biCompression    = BI_RGB;

  std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
  GetDIBits(g_hdcMem, g_hbmMem, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi),
            DIB_RGB_COLORS);

  // Convert the two COLORREFs to the DIB's DWORD representation.
  const DWORD oldPix =
      (GetRValue(oldColor) << 16) | (GetGValue(oldColor) << 8) | GetBValue(oldColor);
  const DWORD newPix =
      (GetRValue(newColor) << 16) | (GetGValue(newColor) << 8) | GetBValue(newColor);

  // Mask off the high (reserved/alpha) byte when comparing so any noise there
  // doesn't cause false negatives on pixels that should match.
  for (auto& p : pixels) {
    if ((p & 0x00FFFFFF) == oldPix) {
      p = (p & 0xFF000000) | newPix;
    }
  }

  SetDIBits(g_hdcMem, g_hbmMem, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi),
            DIB_RGB_COLORS);

  LeaveCriticalSection(&g_paintCS);
}

bool SetNumAnts(const unsigned int num) {
  bool ok              = true;
  unsigned int clamped = num;
  if (clamped > kMaxAntThreads) {
    clamped = kMaxAntThreads;
  }
  if (clamped == 0) {
    clamped = 1;
  }
  g_num_ants = clamped;
  // If the pool is already running (i.e. we're past ShowAnts), resize it to
  // match. Before ShowAnts there is nothing to resize — ShowAnts will spawn
  // the right number of threads using g_num_ants directly.
  if (g_running) {
    if (!EnsureThreadCount(static_cast<int>(clamped))) {
      LOG(ERROR) << L"EnsureThreadCount(" << clamped
                 << L") failed (CreateThread / CreateEvent inside ant thread pool grow path)";
      ok = false;
    }
  }
  return ok;
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
  // The simulation is now actually running — clear the "stopped" hint so
  // the pause/play button knows to say "Pause" / "Resume" instead of
  // "Play" once the user starts interacting.
  g_stopped = false;
  return true;
}

bool TogglePaintAnts(HWND hWnd) {
  bool ok = true;
  if (hWnd == nullptr) {
    LOG(ERROR) << L"TogglePaintAnts: null hWnd";
    return false;
  }
  g_paused = !g_paused;
  // Pause = kill the timer so no more ticks fire. Every active thread sits
  // parked on its tick event, zero CPU. Resume = re-arm the timer and
  // give one immediate pulse so the window doesn't wait up to g_delay ms
  // before redrawing. SyncBgm enforces "audio plays if sound enabled
  // AND ants running", so it covers both the pause and resume sides of
  // BGM in one call — single-step lands here too (it enters the paused
  // branch once, then further single-steps no-op this toggle), so the
  // BGM stays paused until the user un-pauses.
  if (g_paused) {
    KillTimer(hWnd, TIMER_ANTS);
  } else {
    // Drain any pending Custom-Seed placements into the thread slots before
    // re-arming the tick so the very first tick after resume picks up the
    // placed positions rather than the previous (random) ones.
    if (g_place_mode) {
      ApplyPlacements();
    }
    // Once the user resumes, we're no longer in the "stopped" state — the
    // pause/play button should next show "Pause", and a subsequent pause
    // should give "Resume" rather than "Play".
    g_stopped = false;
    SignalAntsTick();
    if (SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr) == 0) {
      LOG(ERROR) << L"TogglePaintAnts: SetTimer failed on resume — "
                    L"simulation will sit idle until something else "
                    L"re-arms the tick source";
      ok = false;
    }
  }
  SyncBgm();
  return ok;
}

void EnterPlaceMode() {
  // Reset the placement list and arm the mode flag. Caller is responsible
  // for pausing the simulation and clearing the canvas (matching the
  // Custom-Seed semantic of "lay out a fresh field by hand").
  // Seed the main thread's rand() once on entry so the first session of a
  // run doesn't always produce the same color sequence — AntThread seeds
  // its own threads but the main thread is otherwise unseeded.
  srand(static_cast<unsigned>(GetTickCount()));
  g_placed_ants_count = 0;
  g_place_mode        = true;
}

void ExitPlaceMode() {
  // Discard pending placements without applying them. The markers we already
  // painted on the canvas stay put — they'll be overpainted by ant trails
  // once the simulation resumes (or wiped by the next IDM_REPAINT).
  g_placed_ants_count = 0;
  g_place_mode        = false;
}

bool PlaceAntAtClient(int clientX, int clientY) {
  if (!g_place_mode) {
    return false;
  }
  if (g_placed_ants_count >= kMaxAntThreads) {
    return false;
  }
  // Window-client → back-buffer coords (the toolbar lives at the top of the
  // client area; the ants canvas starts below it).
  const int bx = clientX;
  const int by = clientY - g_toolbarHeight;
  if (bx < 0 || by < 0) {
    return false;
  }

  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem == nullptr || cxClient <= 0 || cyClient <= 0) {
    LeaveCriticalSection(&g_paintCS);
    return false;
  }
  const int gridW = cxClient / CELL_PX;
  const int gridH = cyClient / CELL_PX;
  if (gridW < 2 || gridH < 2) {
    LeaveCriticalSection(&g_paintCS);
    return false;
  }
  const int cellX = bx / CELL_PX;
  const int cellY = by / CELL_PX;
  if (cellX >= gridW || cellY >= gridH) {
    LeaveCriticalSection(&g_paintCS);
    return false;
  }

  const int px = cellX * CELL_PX;
  const int py = cellY * CELL_PX;
  // Sample the cell BEFORE we paint the marker — the thread that adopts
  // this position needs to know whether it started on background (turn
  // right next tick) or on a path (turn left). The color picker mirrors
  // AntThread's needsPlacement branch: monochrome → match the trail color
  // (ants vanish into their paths, no ant-vs-ant collision); otherwise
  // pick from the magenta/cyan/yellow set so isBlocked sees the marker.
  const COLORREF sampled = GetPixel(g_hdcMem, px, py);
  const bool onBg        = (sampled == g_bkg_color);
  COLORREF antColor;
  if (g_monochrome) {
    antColor = CurrentPathColor();
  } else if (g_ant_color == kRandomAntColor) {
    static const COLORREF kAntColors[3] = {
        RGB_MAGENTA,
        RGB_CYAN,
        RGB_YELLOW,
    };
    antColor = kAntColors[rand() % 3];
  } else {
    antColor = g_ant_color;
  }
  RECT rc     = {px, py, px + CELL_PX, py + CELL_PX};
  HBRUSH hAnt = CreateSolidBrush(antColor);
  FillRect(g_hdcMem, &rc, hAnt);
  DeleteObject(hAnt);
  LeaveCriticalSection(&g_paintCS);

  s_placedAnts[g_placed_ants_count].cellX = cellX;
  s_placedAnts[g_placed_ants_count].cellY = cellY;
  s_placedAnts[g_placed_ants_count].color = antColor;
  s_placedAnts[g_placed_ants_count].onBg  = onBg;
  g_placed_ants_count++;

  RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
  InvalidateRect(mainHwnd, &inval, FALSE);
  return true;
}

bool UndoLastPlacement() {
  bool ok = true;
  if (!g_place_mode) {
    LOG(ERROR) << L"UndoLastPlacement: called outside place mode";
    return false;
  }
  if (g_placed_ants_count <= 0) {
    LOG(ERROR) << L"UndoLastPlacement: no placements to undo";
    return false;
  }
  // Pop the last placement and erase its marker. Place mode always
  // starts from a wiped canvas (IDM_CUSTOMPLACE handler), so every
  // placement cell sits over background — repainting with g_bkg_color
  // restores the cell to its pre-click state.
  const int idx   = g_placed_ants_count - 1;
  const int cellX = s_placedAnts[idx].cellX;
  const int cellY = s_placedAnts[idx].cellY;
  const int px    = cellX * CELL_PX;
  const int py    = cellY * CELL_PX;
  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem != nullptr) {
    RECT rc       = {px, py, px + CELL_PX, py + CELL_PX};
    HBRUSH hBrush = CreateSolidBrush(g_bkg_color);
    FillRect(g_hdcMem, &rc, hBrush);
    DeleteObject(hBrush);
  }
  LeaveCriticalSection(&g_paintCS);
  g_placed_ants_count--;
  RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
  InvalidateRect(mainHwnd, &inval, FALSE);
  return ok;
}

static bool ApplyPlacements() {
  bool ok = true;
  if (!g_place_mode) {
    LOG(ERROR) << L"ApplyPlacements called outside place mode, nothing to apply";
    return false;
  }
  if (g_placed_ants_count > 0) {
    // The placed count becomes the new active ant count. SetNumAnts updates
    // g_num_ants and resizes the thread pool to match — the main.cc caller
    // refreshes the IDM_CONC_N menu radio after this returns.
    if (!SetNumAnts(static_cast<unsigned int>(g_placed_ants_count))) {
      LOG(ERROR) << L"SetNumAnts(" << g_placed_ants_count
                 << L") failed — placed ants may not all have a thread to drive them";
      ok = false;
    }
    for (int i = 0; i < g_placed_ants_count; i++) {
      s_slots[i].placeCellX         = s_placedAnts[i].cellX;
      s_slots[i].placeCellY         = s_placedAnts[i].cellY;
      s_slots[i].placeColor         = s_placedAnts[i].color;
      s_slots[i].placeOnBg          = s_placedAnts[i].onBg;
      s_slots[i].placementRequested = true;
    }
  }
  g_placed_ants_count = 0;
  g_place_mode        = false;
  return ok;
}

INT_PTR CALLBACK CustomDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      // Set icon in titlebar of about dialog
      static const HICON kCustomIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_SMALL));
      SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)kCustomIcon);
      SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)kCustomIcon);
      return TRUE;
    case WM_CLOSE:
      EndDialog(hDlg, TRUE);
      return TRUE;
    case WM_COMMAND: {
      const int cmd = LOWORD(wParam);
      switch (cmd) {
        case IDCANCEL:
          EndDialog(hDlg, IDCANCEL);
          return TRUE;
        case IDOK: {
          // 32 chars is plenty — UINT_MAX in decimal is 10 digits, plus
          // null terminator. ValidateCustomSeed already enforces all-digit
          // input no greater than INT_MAX.
          wchar_t buf[32] = {};
          GetDlgItemTextW(hDlg, IDC_CUSTOMSEED, buf, sizeof(buf) / sizeof(buf[0]));
          if (!ValidateCustomSeed(buf)) {
            ErrorBox(hDlg, L"Custom Seed Validation Error",
                     L"Invalid input — must be a positive integer.");
            // Re-focus the edit so the user can correct without retabbing.
            // Dialog stays open (return TRUE without EndDialog).
            SetFocus(GetDlgItem(hDlg, IDC_CUSTOMSEED));
            return TRUE;
          }
          const unsigned long lseed = wcstoul(buf, nullptr, 10);
          const UINT seed           = static_cast<unsigned int>(lseed);
          CustomSeedAnts(seed);
          EndDialog(hDlg, IDOK);
          return TRUE;
        }
        case IDC_CUSTOMSEED:
          break;
        default:
          break;
      }
    } break;
    default:
      break;
  }
  return FALSE;
}
