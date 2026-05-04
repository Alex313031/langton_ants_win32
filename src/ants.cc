#include "ants.h"

#include <cstring> // memcpy/memset for the state grid
#include <new>     // std::nothrow

#include "cpu.h"
#include "globals.h"
#include "resource.h"
#include "sound.h"
#include "utils.h"

volatile bool g_running = false; // Global ant threads running state
volatile bool g_paused  = false; // Affects g_running, used by IDM_PAUSED
volatile bool g_stopped = true;  // True at startup (no animation yet) and after IDM_STOP. Drives
                                 // "Play" vs "Resume" label on the pause/play toolbar button.

bool g_monochrome = false; // Whether monochrome colors only is enabled

// Fixed ant marker color, or kRandomAntColor for per-ant random pick
COLORREF g_ant_color = kRandomAntColor;

// Initialize to 1, in case something goes wrong at least we draw 1 ant
volatile UINT g_num_ants = 1;

// Default until InitMenuDefaults reads the RC.
unsigned long g_delay = kRealTime;

// Default to the original 2-state Langton's ant. InitMenuDefaults overrides
// from whichever IDM_CLASSIC..IDM_LOGARITHMIC entry is CHECKED in the RC.
AntAlgorithm g_algorithm = AntAlgorithm::Classic;

// Cell grid overlay toggle. Default off; InitMenuDefaults reads the RC's
// IDM_SHOWGRID CHECKED state and may flip it on at startup.
bool g_show_grid = false;

// Toroidal canvas toggle. Default off (ants bounce off the edges); when
// on, off-edge steps wrap to the other side. InitMenuDefaults reads the
// RC's IDM_NOCLIENTBOUNDS CHECKED state to pick the startup value.
bool g_no_client_bounds = false;

// Default ant cell size, controllable from MIN_CELL_PX to MAX_CELL_PX.
// volatile because the main thread mutates it (SetCellSize) while ant
// threads read it every tick - the actual ordering / atomicity guarantee
// comes from g_paintCS holding both writes and reads, but volatile
// documents the runtime mutability and matches the convention used by
// g_paused / g_running / g_num_ants.
volatile int CELL_PX = 6;

// --- Algorithm pattern table ---------------------------------------------
// One entry per AntAlgorithm value. Pattern is the right/left turn string
// shown in the menu; numStates = strlen(pattern). Each cell's "state" is an
// index into pattern[]; the next-state cycle is (state + 1) % numStates.
struct AlgoPattern {
  AntAlgorithm algo;
  const char* pattern;
  int numStates;
};

// Our different algorithm options
static const AlgoPattern kAlgoPatterns[] = {
    {AntAlgorithm::Classic, "RL", 2},
    {AntAlgorithm::Fill, "LRL", 3},
    {AntAlgorithm::Archimedes, "LRRRRLLLRRR", 11},
    {AntAlgorithm::Logarithmic, "RLLLLRRRLLLR", 12},
};

// Returns the entry matching the current g_algorithm, or Classic as a
// defensive fallback if g_algorithm somehow holds an unknown value (would
// only happen on a programming error - the menu only ever sets a known one).
static const AlgoPattern& CurrentAlgoPattern() {
  for (const AlgoPattern& entry : kAlgoPatterns) {
    if (entry.algo == g_algorithm) {
      return entry;
    }
  }
  return kAlgoPatterns[0];
}

// --- Per-cell state grid -------------------------------------------------
// Parallel to the back buffer but one entry per ant cell (CELL_PX × CELL_PX
// pixels). Each entry is the cell's current state index; pattern[state]
// drives the per-tick turn rule. The back buffer only ever shows bg or path
// color so multi-state algorithms work without modifying the color palette
// - the state lives here, the pixels just visualize "visited or not". All
// access is serialized by g_paintCS, same as the back buffer.
static unsigned char* g_state_grid = nullptr;
static int g_state_grid_width      = 0;
static int g_state_grid_height     = 0;

// Resizes (or first-time allocates) the state grid to newWidth × newHeight
// cells. Old cells in [0..oldWidth) × [0..oldHeight) keep their state;
// newly-introduced cells default to state 0 (= background). Returns false
// on allocation failure - the grid is left untouched in that case so the
// next attempt (e.g. the next WM_SIZE) can retry. Caller MUST hold g_paintCS.
static bool ResizeStateGrid(int newWidth, int newHeight) {
  if (newWidth <= 0 || newHeight <= 0) {
    delete[] g_state_grid;
    g_state_grid        = nullptr;
    g_state_grid_width  = 0;
    g_state_grid_height = 0;
    return true;
  }
  if (newWidth == g_state_grid_width && newHeight == g_state_grid_height) {
    return true; // same size, keep existing
  }
  const size_t newCount  = static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight);
  unsigned char* newGrid = new (std::nothrow) unsigned char[newCount]();
  if (newGrid == nullptr) {
    LOG(ERROR) << L"ResizeStateGrid: allocation failed for " << newWidth << L"x" << newHeight;
    return false;
  }
  if (g_state_grid != nullptr) {
    const int copyWidth  = (g_state_grid_width < newWidth) ? g_state_grid_width : newWidth;
    const int copyHeight = (g_state_grid_height < newHeight) ? g_state_grid_height : newHeight;
    for (int rowIdx = 0; rowIdx < copyHeight; ++rowIdx) {
      memcpy(newGrid + static_cast<size_t>(rowIdx) * newWidth,
             g_state_grid + static_cast<size_t>(rowIdx) * g_state_grid_width,
             static_cast<size_t>(copyWidth));
    }
    delete[] g_state_grid;
  }
  g_state_grid        = newGrid;
  g_state_grid_width  = newWidth;
  g_state_grid_height = newHeight;
  return true;
}

// Resets every cell in the state grid to state 0 (background). Paired with
// ClearCanvasToBackground so the visual canvas and the state grid stay
// coherent - in particular this is what makes an algorithm change safe
// (a stale state >= newAlgo.numStates would otherwise read past the
// pattern string). Caller MUST hold g_paintCS.
static void ZeroStateGrid() {
  if (g_state_grid != nullptr && g_state_grid_width > 0 && g_state_grid_height > 0) {
    memset(g_state_grid, 0,
           static_cast<size_t>(g_state_grid_width) * static_cast<size_t>(g_state_grid_height));
  }
}

static AntThreadSlot s_slots[kMaxAntThreads];
static int s_activeCount = 0; // only touched from the main thread

// --- Place-mode state -----------------------------------------------------
// All touched from the main (UI) thread only - set when the user enters
// place mode, populated by PlaceAntAtClient on each click, drained by
// ApplyPlacements when the user resumes. The ant's starting "state" comes
// from the parallel state grid each tick, so the placement record itself
// only carries position + marker color.
struct PlacedAnt {
  int cellX;
  int cellY;
  COLORREF color;
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
// unless the background is white, in which case black. Re-read on every step
// so the ant adapts immediately if the user changes the bg mid-simulation.
// (Existing trails are left alone - see the comment on RecolorBackground.)
static COLORREF CurrentPathColor() {
  if (g_bkg_color == RGB_WHITE) {
    return RGB_BLACK;
  }
  return RGB_WHITE;
}

// Color the back buffer should show for cell state `state`. The longest
// algorithm pattern (Logarithmic) has 12 states, so the table covers
// states 2..11 explicitly; state 0 is bg and state 1 reuses the existing
// CurrentPathColor() so Classic + 2-state behavior is visually unchanged.
//
// Palette constraints (don't change a color without re-checking these):
//   - Avoid magenta / cyan / yellow exactly - those are ant marker colors
//     and isBlocked() keys off them for ant-vs-ant collision detection.
//   - Avoid the six selectable bg colors exactly (white, black, grey,
//     red, green, blue). RecolorBackground swaps oldBg → newBg pixel by
//     pixel; a palette color matching one would get swept away with the bg.
//   - Mid-saturation hues so cells stay visible against any of the
//     selectable backgrounds.
static COLORREF StateColor(unsigned char state) {
  if (state == 0) {
    return g_bkg_color;
  }
  if (state == 1) {
    return CurrentPathColor();
  }
  static const COLORREF kStatePalette[10] = {
      RGB(255, 128, 0),   // 2  - orange
      RGB(255, 192, 64),  // 3  - gold
      RGB(128, 255, 0),   // 4  - lime
      RGB(0, 255, 128),   // 5  - spring
      RGB(0, 160, 160),   // 6  - teal
      RGB(64, 160, 255),  // 7  - sky
      RGB(80, 0, 192),    // 8  - indigo
      RGB(160, 64, 255),  // 9  - purple
      RGB(255, 128, 192), // 10 - pink
      RGB(255, 128, 128), // 11 - salmon
  };
  // Wrap with palette length so an out-of-range state (shouldn't happen
  // - the AntThread step branch already clamps) never reads past the array.
  return kStatePalette[(state - 2) % 10];
}

DWORD WINAPI AntThread(LPVOID pvoid_in) {
  AntThreadSlot* slot = static_cast<AntThreadSlot*>(pvoid_in);
  if (mainHwnd == nullptr || slot == nullptr) {
    return 0x00000001;
  }
  static constexpr DWORD fibonacci = 0x9E3779B9u;
  // Per-ant state, thread-local so no synchronization is needed for it. The
  // shared state (g_hdcMem / the back buffer bitmap) is protected by
  // g_paintCS inside the tick loop.
  //   cellX / cellY - current cell in the CELL_PX grid, negative means
  //                   "needs placement" (first tick, or canvas shrank under
  //                   us and our old cell is out of range).
  //   dir           - 0=N, 1=E, 2=S, 3=W. Right turn = +1, left = +3,
  //                   reverse = +2, all mod 4.
  // rand() on Win32 uses per-thread state, so srand'ing here seeds only
  // this thread's sequence. Mixing in something that varies per slot
  // keeps simultaneous starts distinct - otherwise all kMaxAntThreads
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
  //   - No custom seed: mix GetTickCount() with the thread ID - the
  //     intent there is "different every run", so non-determinism is
  //     a feature, not a bug.
  DWORD seed;
  UINT cSeed          = 0;
  const DWORD slotIdx = static_cast<DWORD>(slot - s_slots);
  // (slotIdx + 1) so slot 0's mix constant isn't zero. With plain slotIdx
  // the XOR for slot 0 is a no-op and the first thread's seed equals the
  // user's raw custom seed (or raw GetCurrentThreadId() in the random
  // branch) - identical input means identical rand() sequence, defeating
  // the per-slot decorrelation the Fibonacci mix exists for.
  const DWORD slotMix = (slotIdx + 1u) * fibonacci;
  if (slot->customSeedReq) {
    cSeed = slot->customSeed;
    seed  = static_cast<DWORD>(cSeed) ^ slotMix;
  } else {
    cSeed = static_cast<UINT>(NULL);
    seed  = (GetCurrentThreadId() * fibonacci) ^ slotMix;
  }
  LOG(DEBUG) << L"AntThread slot=" << static_cast<UINT>(slotIdx + 1u) << L" customSeed=" << cSeed
             << L" seed=" << logging::Hex(seed);

  srand(static_cast<unsigned int>(seed));
  int cellX = -1, cellY = -1;
  int dir = 0;
  // Per-ant marker color. Picked once at placement (see needsPlacement
  // branch below) from {magenta, cyan, yellow} so multiple ants on the
  // canvas are easy to tell apart. Collision detection treats any of
  // those three as "another ant" - see isBlocked in the step branch.
  COLORREF antColor = RGB_MAGENTA;
  // The cell's "state" is read from g_state_grid each tick - no per-thread
  // cache is needed (see the multi-state step branch below).
  // Direction → (dx, dy) in cell units, matching the encoding above.
  static const int kDx[4] = {0, 1, 0, -1};
  static const int kDy[4] = {-1, 0, 1, 0};

  while (true) {
    // Block until SignalAntsTick signals this slot's private event. Auto-reset,
    // so it returns to non-signalled immediately and we block again on the
    // next iteration. Any failure / spurious wake exits the thread.
    if (slot->hTickEvent == nullptr ||
        WaitForSingleObject(slot->hTickEvent, INFINITE) != WAIT_OBJECT_0) {
      LOG(ERROR) << L"AntThread: tick event wait failed (slot " << static_cast<int>(slot - s_slots)
                 << L") - thread exiting...";
      break;
    }
    // Two exit paths: global shutdown OR this individual slot was asked to die
    // (EnsureThreadCount shrinking the pool).
    if (!g_running || slot->exitReq) {
      break;
    }
    // Main thread may have requested a reseed (IDM_REPAINT). Clearing
    // cellX triggers the needsPlacement branch below, which rerolls
    // position, direction, and marker color from the current rand()
    // state - so each reseed produces a fresh layout.
    if (slot->reseedReq) {
      slot->reseedReq = false;
      cellX           = -1;
    }
    // Place-mode handoff. The main thread painted the marker on the canvas
    // already, so we adopt the user-clicked position + the marker's color
    // and skip stepping this tick - the next tick will do a normal Langton
    // step from here, reading the cell's state from g_state_grid. The
    // direction is rolled from rand() (which may have been seeded by a
    // custom seed at thread startup), so a custom seed only varies the
    // direction in place mode; position and color stay user-controlled.
    if (slot->customPlaceReq) {
      slot->customPlaceReq = false;
      cellX                = slot->placeCellX;
      cellY                = slot->placeCellY;
      antColor             = slot->placeColor;
      dir                  = rand() & 3;
      continue;
    }
    // Color-only refresh (Monochrome toggle). Re-pick antColor from the
    // current g_monochrome and overpaint the ant's current cell so the
    // new color shows up immediately (matters when paused - otherwise
    // the next Langton step would draw it anyway). Position and direction
    // are deliberately preserved - this mirrors how the background color
    // menu only swaps pixels and never touches ant draw state. The cell's
    // state in g_state_grid is also untouched.
    // cellX < 0 means we haven't placed yet; the next needsPlacement
    // branch will pick a color naturally, so we skip the paint.
    if (slot->colorRefreshReq) {
      slot->colorRefreshReq = false;
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
          FillRectWithColor(g_hdcMem, rc, antColor);
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

    // Serialize every GDI operation on the back buffer - multiple ant threads
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
          // the grid below our old cell. Roll this ant's marker color,
          // then overpaint. The starting "state" is whatever the state
          // grid already holds at the chosen cell (could be 0 if pristine,
          // or non-zero if a previous ant left a trail there) - the next
          // tick's step branch reads it from the grid directly. No Langton
          // step this tick - next tick starts stepping normally.
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
          const int px = cellX * CELL_PX;
          const int py = cellY * CELL_PX;
          RECT antRc   = {px, py, px + CELL_PX, py + CELL_PX};
          FillRectWithColor(g_hdcMem, antRc, antColor);
          RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
          InvalidateRect(mainHwnd, &inval, FALSE);
        } else {
          // Multi-state Langton step driven by g_algorithm. The cell's
          // state index is in g_state_grid (the back buffer only shows
          // bg vs. path so we can't recover state from the pixel under the
          // ant marker - and even without the marker, multi-state algos
          // would need more colors than we want to introduce). Pattern
          // [state] picks the turn (R or L), state advances by 1 mod
          // numStates, the cell is repainted bg if the new state is 0
          // and path color otherwise.
          const AlgoPattern& algo  = CurrentAlgoPattern();
          const int cellIdx        = cellY * g_state_grid_width + cellX;
          const unsigned char prev = g_state_grid[cellIdx];
          // Defensive clamp: if the grid somehow contains a stale state
          // larger than the current algorithm's pattern (e.g. a missed
          // wipe between an algorithm change and the next tick), treat
          // it as state 0. ZeroStateGrid in ClearCanvasToBackground means
          // this should never trigger in practice.
          const unsigned char state =
              (prev < static_cast<unsigned char>(algo.numStates)) ? prev : 0;
          const char turn               = algo.pattern[state];
          dir                           = (turn == 'R') ? (dir + 1) & 3 : (dir + 3) & 3;
          const unsigned char nextState = static_cast<unsigned char>((state + 1) % algo.numStates);
          g_state_grid[cellIdx]         = nextState;

          const COLORREF trailColor = StateColor(nextState);
          const int px              = cellX * CELL_PX;
          const int py              = cellY * CELL_PX;
          // Overpaint the vacating cell with the new state's color.
          // Removes the ant marker and shows the cell's updated trail.
          RECT trailRc = {px, py, px + CELL_PX, py + CELL_PX};
          FillRectWithColor(g_hdcMem, trailRc, trailColor);

          // Try to step forward. A target cell is "blocked" if it's out of
          // bounds (wall) or currently occupied by another ant (magenta /
          // cyan / yellow marker). On block, reverse direction 180° and
          // try the other way - the same "bounce" rule covers both walls
          // and ant-vs-ant collisions. If the reversed cell is also
          // blocked, stay put for this tick.
          auto isBlocked = [&](int xx, int yy) -> bool {
            if (xx < 0 || xx >= gridW || yy < 0 || yy >= gridH) {
              return true;
            }
            // GetPixel the back buffer (not the state grid) - state-grid
            // entries don't track ant markers, only the cell's trail state.
            const COLORREF sampled = GetPixel(g_hdcMem, xx * CELL_PX, yy * CELL_PX);
            return sampled == RGB_MAGENTA || sampled == RGB_CYAN || sampled == RGB_YELLOW;
          };
          // Computes the candidate next cell from (xx, yy) stepping in
          // direction dd. With g_no_client_bounds the canvas wraps as a
          // torus (off the right edge re-enters on the left, off the
          // bottom re-enters at the top). Without it, OOB stays OOB and
          // isBlocked returns true on it so the bounce rule below kicks
          // in - same path the wall case has always taken.
          // Modulo with negative dividends can return negative in C++,
          // so add gridW/H first to keep the result in [0, gridW)/[0, gridH).
          auto nextCell = [&](int xx, int yy, int dd, int& outX, int& outY) {
            int newX = xx + kDx[dd];
            int newY = yy + kDy[dd];
            if (g_no_client_bounds) {
              newX = ((newX % gridW) + gridW) % gridW;
              newY = ((newY % gridH) + gridH) % gridH;
            }
            outX = newX;
            outY = newY;
          };
          int nx = 0, ny = 0;
          nextCell(cellX, cellY, dir, nx, ny);
          if (isBlocked(nx, ny)) {
            dir = (dir + 2) & 3;
            nextCell(cellX, cellY, dir, nx, ny);
            if (isBlocked(nx, ny)) {
              nx = cellX;
              ny = cellY;
            }
          }
          cellX = nx;
          cellY = ny;

          // Paint the ant on the new cell using this ant's chosen marker
          // color (locked in at placement, see needsPlacement branch).
          // No need to sample anything - the state for next tick will be
          // re-read from g_state_grid at the top of that tick's step branch.
          const int npx = cellX * CELL_PX;
          const int npy = cellY * CELL_PX;
          RECT antRc    = {npx, npy, npx + CELL_PX, npy + CELL_PX};
          FillRectWithColor(g_hdcMem, antRc, antColor);

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
    const int i           = s_activeCount;
    s_slots[i].exitReq    = false;
    s_slots[i].hTickEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
  // can only observe exitReq after a wake, so we SetEvent to force it
  // to run the check. Then join and clean up.
  while (s_activeCount > targetCount) {
    const int i        = s_activeCount - 1;
    s_slots[i].exitReq = true;
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
  // cell - position, direction, and per-cell state stay untouched, so the
  // simulation continues exactly where it was, just dressed in the new
  // color scheme.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].colorRefreshReq = true;
    if (s_slots[i].hTickEvent != nullptr) {
      SetEvent(s_slots[i].hTickEvent);
    }
  }
}

void ReseedAnts(bool pulse) {
  // Flag every active slot so the next tick re-rolls its cellX / cellY /
  // dir / antColor. When pulse is true, also SetEvent each tick event so
  // the reseed runs even while paused - that's the "Repaint now" path.
  // When pulse is false (IDM_STOP), the threads stay parked on their
  // tick events and the wiped canvas stays blank until something else
  // (typically the resume path in TogglePaintAnts) pulses them.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].reseedReq     = true;
    s_slots[i].customSeedReq = false;
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
    LOG(WARN) << L"No ant threads active, nothing to seed.";
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
    UserMessage(std::wstring(L"Using custom seed '") + std::to_wstring(custom_seed) +
                L"' for ant direction.");
  } else {
    UserMessage(std::wstring(L"Using custom seed '") + std::to_wstring(custom_seed) +
                L"' for ant placement, direction, and color.");
  }

  // Stop the timer for the duration of the respawn so no stray WM_TIMER
  // pulses arrive between teardown and the new threads being ready.
  if (mainHwnd != nullptr) {
    KillTimer(mainHwnd, TIMER_ANTS);
  }

  // Tear down every live thread. Mirrors the loop in ShutDownAnts but
  // leaves g_running = true so the freshly-spawned threads below don't
  // immediately exit on their first tick.
  for (int i = 0; i < s_activeCount; i++) {
    s_slots[i].exitReq = true;
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
  // changing the seed "repaints the whole thing"). Splitting the wipe and
  // marker repaint into two locked sections is safe here: every ant thread
  // was torn down above, and we're on the main UI thread so no WM_PAINT /
  // WM_SIZE can race in between.
  ClearCanvasToBackground(cxClient, cyClient);
  if (inPlaceMode) {
    // Their pre-seed color stays - the AntThread customPlaceReq handler
    // re-rolls antColor from the seeded rand() once the simulation resumes,
    // so the moving ant may briefly differ in color until the first Langton
    // step overpaints the marker with the trail color.
    RepaintPlacementMarkers();
  }

  // Stage the seed on the slot scratch fields BEFORE EnsureThreadCount
  // creates the threads - AntThread reads customSeedReq in its
  // startup block, picks up customSeed, and srand's its per-thread rand()
  // from it. Reset the other request flags too so a stale placement /
  // reseed from a prior session can't fire on the very first tick.
  for (int i = 0; i < desiredCount; i++) {
    s_slots[i].customSeedReq  = true;
    s_slots[i].customSeed     = custom_seed;
    s_slots[i].reseedReq      = false;
    s_slots[i].customPlaceReq = false;
  }
  if (!EnsureThreadCount(desiredCount)) {
    LOG(FATAL) << L"EnsureThreadCount(" << desiredCount
               << L") failed during respawn, thread pool corrupted!";
    ok = false;
  }

  if (mainHwnd != nullptr) {
    InvalidateRect(mainHwnd, nullptr, FALSE);
  }

  // Restore the simulation's previous play state. If the user was running,
  // re-arm the timer and pulse so the first tick happens immediately
  // rather than waiting up to g_delay ms. If the user was paused, leave
  // the timer off - the threads sit on their tick events until resume.
  if (wasRunning && mainHwnd != nullptr) {
    SetTimer(mainHwnd, TIMER_ANTS, g_delay, nullptr);
    SignalAntsTick();
  }
  return ok;
}

void ShutDownAnts() {
  KillTimer(mainHwnd, TIMER_ANTS);
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
  // No live threads can race us at this point; safe to free without the lock.
  delete[] g_state_grid;
  g_state_grid        = nullptr;
  g_state_grid_width  = 0;
  g_state_grid_height = 0;
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
  // Fast path: the existing bitmap already matches - keep it, no work,
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
    LOG(FATAL) << L"CreateCompatibleBitmap(" << cx << L"x" << cy
               << L") failed, out of GDI resources.";
    return false;
  }

  // Hold the lock while swapping the bitmap so the ant thread cannot draw into
  // g_hdcMem while we are replacing what it points at.
  EnterCriticalSection(&g_paintCS);
  // Prime hbmNew through a scratch DC: fill with bg, then blit the old
  // back buffer's contents into the top-left. This preserves ant trails
  // across the resize - and also covers any minimize-then-restore path
  // where something fires an intermediate WM_SIZE and triggers this
  // slow branch. On grow, the extra margin stays bg; on shrink, the
  // excess rows / columns of the old bitmap get clipped off.
  HDC hdcScratch         = CreateCompatibleDC(g_hdcMem);
  HBITMAP hbmScratchPrev = static_cast<HBITMAP>(SelectObject(hdcScratch, hbmNew));
  RECT rc                = {0, 0, cx, cy};
  FillRectWithColor(hdcScratch, rc, g_bkg_color);
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
  // Resize the parallel state grid to match the new cell-quantized canvas.
  // Same preserve-old-cells semantics as the bitmap above: existing trails
  // and their states stay aligned across the resize.
  ResizeStateGrid(cx / CELL_PX, cy / CELL_PX);
  LeaveCriticalSection(&g_paintCS);
  return ok;
}

void ClearCanvasToBackground(int cxClient, int cyClient) {
  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
    RECT rc = {0, 0, cxClient, cyClient};
    FillRectWithColor(g_hdcMem, rc, g_bkg_color);
  }
  // Wipe the state grid in lockstep with the visual wipe so an algorithm
  // change never sees stale state >= newAlgo.numStates (which would index
  // past the pattern string in the AntThread tick).
  ZeroStateGrid();
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
  for (DWORD& pixel : pixels) {
    if ((pixel & 0x00FFFFFF) == oldPix) {
      pixel = (pixel & 0xFF000000) | newPix;
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
  // match. Before ShowAnts there is nothing to resize - ShowAnts will spawn
  // the right number of threads using g_num_ants directly.
  if (g_running) {
    if (!EnsureThreadCount(static_cast<int>(clamped))) {
      LOG(FATAL) << L"EnsureThreadCount(" << clamped
                 << L") failed: CreateThread / CreateEvent inside ant thread pool grow path error.";
      ok = false;
    }
  }
  return ok;
}

bool ShowAnts() {
  if (g_num_ants == 0 || g_delay == 0) {
    LOG(FATAL) << L"Number of ants or delay Out Of Bounds!";
    return false;
  }

  // Spin up the initial thread pool matching the current Num Ants
  // setting. Each thread owns its own auto-reset wake event and blocks on
  // it until SignalAntsTick (driven by WM_TIMER) says "go."
  g_running = true;
  if (!EnsureThreadCount(static_cast<int>(g_num_ants))) {
    ShutDownAnts();
    return false;
  }

  // Start the timer that drives drawing. WM_TIMER fires every g_delay ms
  // and, via SignalAntsTick, pulses every active thread's tick event once.
  if (!SetTimer(mainHwnd, TIMER_ANTS, g_delay, nullptr)) {
    LOG(ERROR) << L"SetTimer failed! Tearing down ant thread pool...";
    ShutDownAnts();
    return false;
  }
  // The simulation is now actually running - clear the "stopped" hint so
  // the pause/play button knows to say "Pause" / "Resume" instead of
  // "Play" once the user starts interacting.
  g_stopped = false;
  return true;
}

bool TogglePaintAnts(HWND hWnd) {
  bool ok = true;
  if (hWnd == nullptr) {
    LOG(FATAL) << L"hWnd was NULL!";
    return false;
  }
  g_paused = !g_paused;
  // Pause = kill the timer so no more ticks fire. Every active thread sits
  // parked on its tick event, zero CPU. Resume = re-arm the timer and
  // give one immediate pulse so the window doesn't wait up to g_delay ms
  // before redrawing. SyncBgm enforces "audio plays if sound enabled
  // AND ants running", so it covers both the pause and resume sides of
  // BGM in one call - single-step lands here too (it enters the paused
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
    // Once the user resumes, we're no longer in the "stopped" state - the
    // pause/play button should next show "Pause", and a subsequent pause
    // should give "Resume" rather than "Play".
    g_stopped = false;
    SignalAntsTick();
    if (SetTimer(hWnd, TIMER_ANTS, g_delay, nullptr) == 0) {
      LOG(ERROR) << L"SetTimer failed on resume: simulation will sit idle"
                    L" until something else re-arms the tick source!";
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
  // run doesn't always produce the same color sequence - AntThread seeds
  // its own threads but the main thread is otherwise unseeded.
  srand(static_cast<unsigned>(GetTickCount()));
  g_placed_ants_count = 0;
  g_place_mode        = true;
}

void ExitPlaceMode() {
  // Discard pending placements without applying them. The markers we already
  // painted on the canvas stay put - they'll be overpainted by ant trails
  // once the simulation resumes (or wiped by the next IDM_REPAINT).
  g_placed_ants_count = 0;
  g_place_mode        = false;
}

bool PlaceAntAtClient(int clientX, int clientY) {
  if (!g_place_mode) {
    return false;
  }
  if (g_placed_ants_count >= kMaxAntThreads) {
    LOG(WARN) << L"Place cap hit (" << kMaxAntThreads << L"): click ignored.";
    return false;
  }
  // Window-client → back-buffer coords (the toolbar lives at the top of the
  // client area; the ants canvas starts below it).
  const int bx = clientX;
  const int by = clientY - g_toolbarHeight;
  if (bx < 0 || by < 0) {
    LOG(DEBUG) << L"Place click ignored: click in toolbar area.";
    return false;
  }

  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem == nullptr || cxClient <= 0 || cyClient <= 0) {
    LeaveCriticalSection(&g_paintCS);
    LOG(ERROR) << L"Place click ignored: no canvas (minimized?)";
    return false;
  }
  const int gridW = cxClient / CELL_PX;
  const int gridH = cyClient / CELL_PX;
  if (gridW < 2 || gridH < 2) {
    LeaveCriticalSection(&g_paintCS);
    LOG(WARN) << L"Place click ignored: grid too small (" << gridW << L"x" << gridH << L")...";
    return false;
  }
  const int cellX = bx / CELL_PX;
  const int cellY = by / CELL_PX;
  if (cellX >= gridW || cellY >= gridH) {
    LeaveCriticalSection(&g_paintCS);
    LOG(WARN) << L"Place click ignored: cell (" << cellX << L"," << cellY << L") outside grid ("
              << gridW << L"x" << gridH << L")!";
    return false;
  }

  const int px = cellX * CELL_PX;
  const int py = cellY * CELL_PX;
  // Color picker mirrors AntThread's needsPlacement branch: monochrome →
  // match the trail color (ants vanish into their paths, no ant-vs-ant
  // collision); otherwise pick from the magenta/cyan/yellow set so
  // isBlocked sees the marker. The starting "state" the thread reads on
  // its first step comes from g_state_grid at this cell, no extra field
  // recorded here.
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
  RECT rc = {px, py, px + CELL_PX, py + CELL_PX};
  FillRectWithColor(g_hdcMem, rc, antColor);
  LeaveCriticalSection(&g_paintCS);

  s_placedAnts[g_placed_ants_count].cellX = cellX;
  s_placedAnts[g_placed_ants_count].cellY = cellY;
  s_placedAnts[g_placed_ants_count].color = antColor;
  g_placed_ants_count++;

  RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
  InvalidateRect(mainHwnd, &inval, FALSE);
  UserMessage(std::wstring(L"Ant placed at cell (") + std::to_wstring(cellX) + L"," +
              std::to_wstring(cellY) + L"). " + std::to_wstring(g_placed_ants_count) + L" of " +
              std::to_wstring(kMaxAntThreads) + L" threads remain.");
  return true;
}

bool UndoLastPlacement() {
  bool ok = true;
  if (!g_place_mode) {
    LOG(ERROR) << L"UndoLastPlacement() called outside place mode!";
    return false;
  }
  if (g_placed_ants_count <= 0) {
    LOG(WARN) << L"No ant placements to undo...";
    return false;
  }
  // Pop the last placement and erase its marker. Place mode always
  // starts from a wiped canvas (IDM_CUSTOMPLACE handler), so every
  // placement cell sits over background - repainting with g_bkg_color
  // restores the cell to its pre-click state.
  const int idx   = g_placed_ants_count - 1;
  const int cellX = s_placedAnts[idx].cellX;
  const int cellY = s_placedAnts[idx].cellY;
  const int px    = cellX * CELL_PX;
  const int py    = cellY * CELL_PX;
  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem != nullptr) {
    RECT rc = {px, py, px + CELL_PX, py + CELL_PX};
    FillRectWithColor(g_hdcMem, rc, g_bkg_color);
  }
  LeaveCriticalSection(&g_paintCS);
  g_placed_ants_count--;
  RECT inval = {px, py + g_toolbarHeight, px + CELL_PX, py + CELL_PX + g_toolbarHeight};
  InvalidateRect(mainHwnd, &inval, FALSE);
  UserMessage(std::wstring(L"Undid placement at cell (") + std::to_wstring(cellX) + L"," +
              std::to_wstring(cellY) + L"). " + std::to_wstring(g_placed_ants_count) + L" of " +
              std::to_wstring(kMaxAntThreads) + L" threads remain.");
  return ok;
}

void RepaintPlacementMarkers() {
  if (g_placed_ants_count <= 0) {
    return;
  }
  EnterCriticalSection(&g_paintCS);
  if (g_hdcMem != nullptr && g_hbmMem != nullptr) {
    for (int placeIdx = 0; placeIdx < g_placed_ants_count; placeIdx++) {
      const PlacedAnt& placed = s_placedAnts[placeIdx];
      const int px            = placed.cellX * CELL_PX;
      const int py            = placed.cellY * CELL_PX;
      RECT markerRc           = {px, py, px + CELL_PX, py + CELL_PX};
      FillRectWithColor(g_hdcMem, markerRc, placed.color);
    }
  }
  LeaveCriticalSection(&g_paintCS);
}

static bool ApplyPlacements() {
  bool ok = true;
  if (!g_place_mode) {
    LOG(ERROR) << L"ApplyPlacements() called outside place mode: nothing to apply!";
    return false;
  }
  if (g_placed_ants_count > 0) {
    // The placed count becomes the new active ant count. SetNumAnts updates
    // g_num_ants and resizes the thread pool to match - the main.cc caller
    // refreshes the IDM_CONC_N menu radio after this returns.
    if (!SetNumAnts(static_cast<unsigned int>(g_placed_ants_count))) {
      LOG(ERROR) << L"SetNumAnts(" << g_placed_ants_count
                 << L") failed! Placed ants may not all have a thread to drive them...";
      ok = false;
    }
    for (int i = 0; i < g_placed_ants_count; i++) {
      s_slots[i].placeCellX     = s_placedAnts[i].cellX;
      s_slots[i].placeCellY     = s_placedAnts[i].cellY;
      s_slots[i].placeColor     = s_placedAnts[i].color;
      s_slots[i].customPlaceReq = true;
    }
  }
  g_placed_ants_count = 0;
  g_place_mode        = false;
  return ok;
}

INT_PTR CALLBACK CustomSeedDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      // Set icon in titlebar of custom seed dialog
      SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)kSmallIcon);
      SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)kSmallIcon);
      return TRUE;
    case WM_CLOSE:
      LOG(INFO) << L"Custom Seed dialog cancelled by user";
      EndDialog(hDlg, TRUE);
      return TRUE;
    case WM_COMMAND: {
      const int cmd = LOWORD(wParam);
      switch (cmd) {
        case IDCANCEL:
          LOG(INFO) << L"Custom Seed dialog cancelled by user";
          EndDialog(hDlg, IDCANCEL);
          return TRUE;
        case IDOK: {
          // 32 chars is plenty - UINT_MAX in decimal is 10 digits, plus
          // null terminator. ValidateCustomSeed already enforces all-digit
          // input no greater than INT_MAX.
          wchar_t buf[32] = {};
          GetDlgItemTextW(hDlg, IDC_CUSTOMSEED, buf, sizeof(buf) / sizeof(buf[0]));
          if (!ValidateCustomSeed(buf)) {
            ErrorBox(hDlg, L"Custom Ant Seed Validation Error",
                     L"Invalid input - must be a positive integer.");
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

INT_PTR CALLBACK CustomNumDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      // Set icon in titlebar of custom num dialog
      SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)kSmallIcon);
      SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)kSmallIcon);
      return TRUE;
    case WM_CLOSE:
      LOG(INFO) << L"Custom Num dialog cancelled by user";
      EndDialog(hDlg, TRUE);
      return TRUE;
    case WM_COMMAND: {
      const int cmd = LOWORD(wParam);
      switch (cmd) {
        case IDCANCEL:
          LOG(INFO) << L"Custom Num dialog cancelled by user";
          EndDialog(hDlg, IDCANCEL);
          return TRUE;
        case IDOK: {
          // 9 chars is plenty - 8, plus null terminator.
          // ValidateCustomNum already enforces only 1 up to kMaxAntThreads (not more than 256).
          wchar_t buf[9] = {};
          GetDlgItemTextW(hDlg, IDC_CUSTOMNUM, buf, sizeof(buf) / sizeof(buf[0]));
          if (!ValidateCustomNum(buf)) {
            ErrorBox(hDlg, L"Custom Number Validation Error",
                     L"Invalid input - must be between 1 - 128.");
            // Re-focus the edit so the user can correct without retabbing.
            // Dialog stays open (return TRUE without EndDialog).
            SetFocus(GetDlgItem(hDlg, IDC_CUSTOMNUM));
            return TRUE;
          }
          const unsigned long lnum = wcstoul(buf, nullptr, 10);
          const UINT numAnts       = static_cast<unsigned int>(lnum);
          SetNumAnts(numAnts);
          // Reflect the new count on the menu: a typed value of 1-16 still
          // lights up the matching IDM_CONC_N radio (so picking "5" via the
          // dialog looks the same as picking "5" via the menu); 17-128 lands
          // on IDM_CONC_CUSTOM with the radios cleared.
          SetNumAntsCheck(numAnts);
          UserMessage(std::wstring(L"Number of ants changed to ") + std::to_wstring(numAnts) +
                      L".");
          EndDialog(hDlg, IDOK);
          return TRUE;
        }
        case IDC_CUSTOMNUM:
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

INT_PTR CALLBACK CustomCellSizeDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      // Set icon in titlebar of custom cell size dialog
      SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)kSmallIcon);
      SendMessageW(hDlg, WM_SETICON, ICON_BIG, (LPARAM)kSmallIcon);
      return TRUE;
    case WM_CLOSE:
      LOG(INFO) << L"Cell Size dialog cancelled by user";
      EndDialog(hDlg, TRUE);
      return TRUE;
    case WM_COMMAND: {
      const int cmd = LOWORD(wParam);
      switch (cmd) {
        case IDCANCEL:
          LOG(INFO) << L"Cell Size dialog cancelled by user";
          EndDialog(hDlg, IDCANCEL);
          return TRUE;
        case IDOK: {
          // 4 chars is plenty - 3, plus null terminator.
          // ValidateCellSize already enforces only 2 up to 48.
          wchar_t buf[9] = {};
          GetDlgItemTextW(hDlg, IDC_CUSTOMCELLSIZE, buf, sizeof(buf) / sizeof(buf[0]));
          if (!ValidateCellSize(buf)) {
            static const std::wstring msg =
                L"Invalid input - must be between " + std::to_wstring(MIN_CELL_PX)
                + L" - " + std::to_wstring(MAX_CELL_PX) + L".";
            ErrorBox(hDlg, L"Custom Cell Size Validation Error", msg);
            // Re-focus the edit so the user can correct without retabbing.
            // Dialog stays open (return TRUE without EndDialog).
            SetFocus(GetDlgItem(hDlg, IDC_CUSTOMCELLSIZE));
            return TRUE;
          }
          const unsigned long lnum = wcstoul(buf, nullptr, 10);
          const int cellSize       = static_cast<int>(lnum);
          SetCellSize(cellSize);
          EndDialog(hDlg, IDOK);
          return TRUE;
        }
        case IDC_CUSTOMCELLSIZE:
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

void SetCellSize(const int size) {
  DCHECK(size >= MIN_CELL_PX);
  DCHECK(size <= MAX_CELL_PX);
  if (g_place_mode) {
    ExitPlaceMode();
  }
  // CELL_PX and the state grid have to change together: the grid is sized
  // in cells (cxClient / CELL_PX), so a torn pair would let an ant thread
  // compute gridW from the new CELL_PX and index into the still-old-sized
  // grid - out of bounds when CELL_PX shrinks. Hold g_paintCS across both
  // the assignment and the resize so the AntThread tick (which acquires
  // the same lock) can never observe the inconsistency.
  EnterCriticalSection(&g_paintCS);
  CELL_PX = size;
  ResizeStateGrid(cxClient / CELL_PX, cyClient / CELL_PX);
  LeaveCriticalSection(&g_paintCS);
  // Wipe the canvas + zero the (newly-resized) state grid. ClearCanvasToBackground
  // re-acquires the lock; CRITICAL_SECTION is recursive on Windows so even
  // re-entering on the same thread would be safe, but we already released above.
  ClearCanvasToBackground(cxClient, cyClient);
  ReseedAnts();
  InvalidateRect(mainHwnd, nullptr, FALSE);
  UserMessage(std::wstring(L"Cell Size changed to ") + std::to_wstring(size) + L".");
}
