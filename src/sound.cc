#include "sound.h"

#include <logging.h>

#include "globals.h"
#include "resource.h"
#include "utils.h" // GetExeDir for the non-embedded file-source path

// User preference, NOT actual playback state. See sound.h.
volatile bool g_playsound = false;

// Tracks whether MCI is currently playing (vs paused / stopped / unopened).
// Main-thread only - written exclusively by SyncBgm/ToggleSound after the
// PostBgmSync call has returned, so the worker never touches it. Lets
// SyncBgm skip the work when audio is already in the right state.
static bool s_audio_playing = false;

// ==========================================================================
// BGM is driven by a dedicated worker thread that owns the MCI device
// end-to-end.
//
// Why a worker thread at all: mciSendStringW("play") blocks its caller
// for ~10-50ms while the waveform driver stages buffers and starts the
// mixer stream. When that call lived on the main thread (inside the
// MM_MCINOTIFY loop-back handler) every loop iteration visibly froze
// the ants canvas - especially at hyper/realtime speeds where a frame
// is 10 ms or less. Moving it to a worker keeps the UI smooth.
//
// Why the worker owns open + every other MCI op (not just the replay):
// some MCI drivers are thread-affinity-bound - a command sent from a
// thread different than the one that opened the device sometimes
// succeeds but silently drops the subsequent notify callback. Opening
// the device on the worker and issuing every command (play, pause,
// resume, stop, close) from the same thread avoids that entire class
// of issue.
//
// Why the worker has its own hidden window: `play ... notify` tells
// MCI to PostMessage(MM_MCINOTIFY) to a caller-chosen HWND when playback
// finishes. Targeting a hidden window owned by the worker means the
// loop re-issue ("play from 0 notify") is handled inside the worker's
// own message loop - main thread is never in the loop path at all.
//
// Main thread talks to the worker through a single-slot command queue
// protected by s_bgmCS. User-triggered ops (Play / Pause / Stop) are
// sync - main posts the command, signals s_bgmCmdEvent, and waits on
// s_bgmDoneEvent. The loop replay needs no main-thread involvement, so
// there is no async post path.
// ==========================================================================

// ---------- Command queue (accessed from both threads under s_bgmCS) ------

enum class BgmCmd {
  None,
  Play, // open if device is closed; resume if it's paused
  Pause,
  Stop, // stop + close + temp-file cleanup
};

struct BgmCmdSlot {
  BgmCmd cmd = BgmCmd::None;
  std::wstring wav;
  bool use_embedded = false;
};

static CRITICAL_SECTION s_bgmCS;
static BgmCmdSlot s_slot;
static bool s_lastResult     = false;
static HANDLE s_bgmCmdEvent  = nullptr; // auto-reset: "cmd pending"
static HANDLE s_bgmDoneEvent = nullptr; // auto-reset: "sync cmd done"
static HANDLE s_bgmExitEvent = nullptr; // manual-reset: "shut down"
static HANDLE s_bgmInitEvent = nullptr; // manual-reset: "worker ready"
static HANDLE s_bgmWorker    = nullptr;
static bool s_bgmInit        = false;
static bool s_bgmInitOk      = false; // worker setup succeeded

// ---------- Worker-thread-only MCI state ----------------------------------
// Touched only from BgmWorkerProc; no synchronization needed.

static HWND s_bgmHwnd   = nullptr; // hidden notify target
static bool s_mciOpened = false;
static std::wstring s_embeddedTempPath;
static const wchar_t kMciBgmAlias[]    = L"langton_ants_bgm";
static const wchar_t kBgmHiddenClass[] = L"LangtonAntsBgmHidden";

// Convert an MCIERROR code into a human-readable string using
// mciGetErrorStringW, the MCI-provided lookup. Used only for logging.
// Falls back to "unknown MCI error <num>" when MCI doesn't recognize its
// own error code, so the calling log lines never print an empty trailer.
static std::wstring MciErrText(MCIERROR err) {
  wchar_t buf[256] = {};
  mciGetErrorStringW(err, buf, sizeof(buf) / sizeof(buf[0]));
  if (buf[0] == L'\0') {
    return L"unknown MCI error " + std::to_wstring(err);
  }
  return std::wstring(buf);
}

// Materializes the IDR_BGM_WAVE resource to a file in the user's temp
// directory and returns that path (empty on failure). MCI's string API can
// only open named files - there is no "play from memory" form - so when
// we want to play the embedded WAV we have to stage it on disk first.
// Windows' FindResource returns a pointer to the PE-loaded resource bytes
// without extra allocation; we just WriteFile them out.
static std::wstring ExtractEmbeddedWavToTemp() {
  HRSRC hRsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_BGM_WAVE), L"WAVE");
  if (hRsrc == nullptr) {
    LOG(ERROR) << L"FindResourceW IDR_BGM_WAVE failed";
    return std::wstring();
  }
  HGLOBAL hGlob = LoadResource(nullptr, hRsrc);
  if (hGlob == nullptr) {
    LOG(ERROR) << L"LoadResource IDR_BGM_WAVE failed";
    return std::wstring();
  }
  const LPVOID pData = LockResource(hGlob);
  const DWORD size   = SizeofResource(nullptr, hRsrc);
  if (pData == nullptr || size == 0) {
    LOG(ERROR) << L"LockResource/SizeofResource returned empty for IDR_BGM_WAVE";
    return std::wstring();
  }

  wchar_t tempDir[MAX_PATH] = {};
  DWORD n                   = GetTempPathW(MAX_PATH, tempDir);
  if (n == 0 || n > MAX_PATH) {
    LOG(ERROR) << L"GetTempPathW failed";
    return std::wstring();
  }
  std::wstring tempPath = std::wstring(tempDir) + L"langton_ants_bgm.wav";

  HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << L"CreateFileW temp failed for " << tempPath;
    return std::wstring();
  }
  DWORD written = 0;
  BOOL ok       = WriteFile(hFile, pData, size, &written, nullptr);
  CloseHandle(hFile);
  if (!ok || written != size) {
    LOG(ERROR) << L"WriteFile temp WAV short-write: " << written << L"/" << size;
    DeleteFileW(tempPath.c_str());
    return std::wstring();
  }
  return tempPath;
}

// ---------- Worker-side MCI operations ------------------------------------

// First-time open + play from position 0. `file` is the path MCI opens,
// either the user-supplied name resolved against the exe dir or the temp
// file we materialize from IDR_BGM_WAVE. Passes s_bgmHwnd as the notify
// callback so MM_MCINOTIFY on completion routes back into this thread's
// message loop.
static bool WorkerOpenPlay(const std::wstring& wav_file, bool use_embedded) {
  if (!use_embedded && wav_file.empty()) {
    return false;
  }
  std::wstring file;
  if (use_embedded) {
    file = ExtractEmbeddedWavToTemp();
    if (file.empty()) {
      return false;
    }
    s_embeddedTempPath = file;
  } else {
    const std::wstring cwd = GetExeDir();
    if (cwd.empty()) {
      return false;
    }
    file = cwd + wav_file;
  }
  // Quoting the path keeps spaces or punctuation in the exe dir from
  // splitting the command into separate tokens. `type waveaudio` pins the
  // open to the waveform driver regardless of the file extension, making
  // the open deterministic.
  std::wstring openCmd = L"open \"";
  openCmd += file;
  openCmd += L"\" type waveaudio alias langton_ants_bgm";
  MCIERROR err = mciSendStringW(openCmd.c_str(), nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI open failed for " << file << L": " << MciErrText(err);
    if (!s_embeddedTempPath.empty()) {
      DeleteFileW(s_embeddedTempPath.c_str());
      s_embeddedTempPath.clear();
    }
    return false;
  }
  s_mciOpened = true;

  // MCIWAVE's `play` does NOT support `repeat` (unlike MCIAVI / MCICDA),
  // so looping is done manually: on every successful completion MCI
  // posts MM_MCINOTIFY to s_bgmHwnd and BgmHiddenWndProc calls
  // WorkerReplay, which re-issues `play from 0 notify` against the same
  // callback HWND.
  err = mciSendStringW(L"play langton_ants_bgm notify", nullptr, 0, s_bgmHwnd);
  if (err != 0) {
    LOG(ERROR) << L"MCI play failed: " << MciErrText(err);
    mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
    s_mciOpened = false;
    if (!s_embeddedTempPath.empty()) {
      DeleteFileW(s_embeddedTempPath.c_str());
      s_embeddedTempPath.clear();
    }
    return false;
  }
  return true;
}

static bool WorkerResume() {
  if (!s_mciOpened) {
    return false;
  }
  // `resume` continues from the current playback position. Do NOT re-issue
  // `play` here - that would reset to 0 and double-register the notify
  // callback.
  MCIERROR err = mciSendStringW(L"resume langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI resume failed: " << MciErrText(err);
    return false;
  }
  return true;
}

static bool WorkerPause() {
  if (!s_mciOpened) {
    return true;
  }
  // `pause` suspends playback without resetting position - the pending
  // `notify` registration stays alive so MM_MCINOTIFY still fires on the
  // eventual natural completion after a subsequent resume.
  MCIERROR err = mciSendStringW(L"pause langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI pause failed: " << MciErrText(err);
  }
  return true;
}

static bool WorkerStop() {
  if (!s_mciOpened) {
    return true;
  }
  // Full tear-down: stop halts playback, close releases the waveform
  // device so a subsequent open can succeed cleanly.
  MCIERROR err = mciSendStringW(L"stop langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI stop failed: " << MciErrText(err);
  }
  err = mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI close failed: " << MciErrText(err);
  }
  s_mciOpened = false;
  if (!s_embeddedTempPath.empty()) {
    DeleteFileW(s_embeddedTempPath.c_str());
    s_embeddedTempPath.clear();
  }
  return true;
}

// Loop re-issue. Called from BgmHiddenWndProc when MCI fires a successful
// MM_MCINOTIFY for the prior play. `from 0` is REQUIRED - after natural
// completion MCI leaves the cursor at EOF, so a bare `play` would be a
// silent no-op. The `notify` param re-registers s_bgmHwnd so the NEXT
// completion also lands in our message loop.
static void WorkerReplay() {
  // Loop only while audio should still be playing right now - i.e., user
  // wants sound on AND the sim isn't paused. If a Pause came in between
  // the original play start and the MM_MCINOTIFY for that play, we want
  // to bail rather than restart. (Reading these volatile bools from the
  // worker is benign - both are written by the main thread as plain
  // assignments, and we only need a snapshot here.)
  if (!s_mciOpened || !g_playsound || g_paused) {
    return;
  }
  MCIERROR err = mciSendStringW(L"play langton_ants_bgm from 0 notify", nullptr, 0, s_bgmHwnd);
  if (err != 0) {
    LOG(ERROR) << L"MCI replay failed: " << MciErrText(err);
  }
}

// Dispatch for BgmCmd values. Runs on the worker thread.
static bool ProcessCmd(const BgmCmdSlot& slot) {
  switch (slot.cmd) {
    case BgmCmd::Play:
      return s_mciOpened ? WorkerResume() : WorkerOpenPlay(slot.wav, slot.use_embedded);
    case BgmCmd::Pause:
      return WorkerPause();
    case BgmCmd::Stop:
      return WorkerStop();
    default:
      return false;
  }
}

// Hidden window procedure. Runs on the worker thread when PeekMessage /
// DispatchMessage below fires MM_MCINOTIFY that MCI posted to s_bgmHwnd.
//
// wParam values we might see: MCI_NOTIFY_SUCCESSFUL (natural end-of-clip
// - we loop), MCI_NOTIFY_SUPERSEDED (another play took over - do nothing),
// MCI_NOTIFY_ABORTED (stop/close happened - do nothing), MCI_NOTIFY_FAILURE
// (driver error - do nothing). The g_playsound guard in WorkerReplay also
// protects against a late-arriving SUCCESSFUL notification that races a
// user Pause.
static LRESULT CALLBACK BgmHiddenWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == MM_MCINOTIFY) {
    if (wp == MCI_NOTIFY_SUCCESSFUL) {
      WorkerReplay();
    }
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wp, lp);
}

// Worker thread entry. Sets up the hidden notify window, signals init
// completion, then loops on MsgWaitForMultipleObjects handling:
//   - s_bgmExitEvent (manual-reset): break out and shut down.
//   - s_bgmCmdEvent  (auto-reset)  : main thread posted a command.
//   - window messages              : MCI notifications to s_bgmHwnd.
static DWORD WINAPI BgmWorkerProc(LPVOID) {
  WNDCLASSW wc     = {};
  wc.lpfnWndProc   = BgmHiddenWndProc;
  wc.hInstance     = GetModuleHandleW(nullptr);
  wc.lpszClassName = kBgmHiddenClass;
  ATOM classAtom   = RegisterClassW(&wc);
  if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LOG(ERROR) << L"BGM RegisterClass failed";
    s_bgmInitOk = false;
    SetEvent(s_bgmInitEvent);
    return 1;
  }
  // HWND_MESSAGE parent makes it an invisible message-only window - no
  // z-order, no taskbar, no focus changes. Pure PostMessage target.
  s_bgmHwnd = CreateWindowW(kBgmHiddenClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            wc.hInstance, nullptr);
  if (s_bgmHwnd == nullptr) {
    LOG(ERROR) << L"BGM CreateWindow failed";
    UnregisterClassW(kBgmHiddenClass, wc.hInstance);
    s_bgmInitOk = false;
    SetEvent(s_bgmInitEvent);
    return 1;
  }
  s_bgmInitOk = true;
  SetEvent(s_bgmInitEvent);

  HANDLE handles[2] = {s_bgmExitEvent, s_bgmCmdEvent};
  while (true) {
    DWORD r = MsgWaitForMultipleObjects(2, handles, FALSE, INFINITE, QS_ALLINPUT);
    if (r == WAIT_OBJECT_0) {
      break; // exit event
    }
    if (r == WAIT_OBJECT_0 + 1) {
      // A command from main thread. Drain the slot under the CS.
      BgmCmdSlot slot;
      EnterCriticalSection(&s_bgmCS);
      slot   = s_slot;
      s_slot = BgmCmdSlot{};
      LeaveCriticalSection(&s_bgmCS);
      if (slot.cmd != BgmCmd::None) {
        bool result = ProcessCmd(slot);
        EnterCriticalSection(&s_bgmCS);
        s_lastResult = result;
        LeaveCriticalSection(&s_bgmCS);
        SetEvent(s_bgmDoneEvent);
      }
    } else if (r == WAIT_OBJECT_0 + 2) {
      // Window-message queue has something - dispatch everything pending.
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    } else {
      LOG(ERROR) << L"BGM worker MsgWait returned " << r;
      break;
    }
  }

  if (s_bgmHwnd != nullptr) {
    DestroyWindow(s_bgmHwnd);
    s_bgmHwnd = nullptr;
  }
  UnregisterClassW(kBgmHiddenClass, wc.hInstance);
  return 0;
}

// ---------- Post helper (main thread) -------------------------------------

// Post a command and wait for the worker to finish processing it. Returns
// the worker's bool result. Main thread only.
static bool PostBgmSync(BgmCmd cmd,
                        const std::wstring& wav = std::wstring(),
                        bool use_embedded       = false) {
  if (!s_bgmInit) {
    return false;
  }
  EnterCriticalSection(&s_bgmCS);
  s_slot.cmd          = cmd;
  s_slot.wav          = wav;
  s_slot.use_embedded = use_embedded;
  LeaveCriticalSection(&s_bgmCS);
  SetEvent(s_bgmCmdEvent);
  WaitForSingleObject(s_bgmDoneEvent, INFINITE);
  EnterCriticalSection(&s_bgmCS);
  bool result = s_lastResult;
  LeaveCriticalSection(&s_bgmCS);
  return result;
}

// ---------- Public API ----------------------------------------------------

bool PlayWavFile(const std::wstring& wav_file, bool use_embedded) {
  return PostBgmSync(BgmCmd::Play, wav_file, use_embedded);
}

bool PauseWavFile() {
  return PostBgmSync(BgmCmd::Pause);
}

bool StopPlayWav() {
  // Hard tear-down. SyncBgm is the normal pause path; this one is reserved
  // for full lifecycle stops (app shutdown, IDM_TESTTRAP). Resets the
  // local "is MCI playing" flag so the next SyncBgm call correctly
  // re-opens the device instead of trying to resume.
  s_audio_playing = false;
  return PostBgmSync(BgmCmd::Stop);
}

bool SyncBgm() {
  // Single source of truth: audio plays if the user wants it AND the
  // simulation is currently running. If audio is already in the right
  // state, do nothing.
  const bool desired = g_playsound && !g_paused;
  if (desired == s_audio_playing) {
    return true;
  }
  if (desired) {
    if (PlayWavFile(sound_file, kUseEmbeddedBgm)) {
      s_audio_playing = true;
      return true;
    }
    // Open / play failed - leave s_audio_playing false; caller sees the
    // failure and can react (e.g., clear g_playsound + refresh the UI).
    return false;
  }
  // desired == false, audio currently playing → pause. PauseWavFile is
  // a no-op when the device isn't open (early-returns true) so we treat
  // the transition as successful even in pathological "play failed
  // earlier" states.
  PauseWavFile();
  s_audio_playing = false;
  return true;
}

bool ToggleSound() {
  // Flip the user pref, then let SyncBgm decide what (if anything) needs
  // to happen at the MCI layer. This is the only place outside SyncBgm
  // itself that mutates g_playsound at runtime. If the resulting Play
  // fails (missing WAV, driver error), revert the flip so the UI doesn't
  // display "sound on" while audio is actually silent.
  g_playsound = !g_playsound;
  if (!SyncBgm()) {
    g_playsound = !g_playsound;
    return false;
  }
  return true;
}

// ---------- Lifecycle -----------------------------------------------------

bool InitBgm() {
  bool ok = true;
  if (s_bgmInit) {
    return true; // already initialized - treat as success
  }
  InitializeCriticalSection(&s_bgmCS);
  s_bgmCmdEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
  s_bgmDoneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
  s_bgmExitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
  s_bgmInitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
  if (!s_bgmCmdEvent || !s_bgmDoneEvent || !s_bgmExitEvent || !s_bgmInitEvent) {
    LOG(ERROR) << L"CreateEventW failed for one or more BGM sync events";
    return false;
  }
  s_bgmWorker = CreateThread(nullptr, 0, BgmWorkerProc, nullptr, 0, nullptr);
  if (!s_bgmWorker) {
    LOG(ERROR) << L"CreateThread for BGM worker failed";
    return false;
  }
  // The worker does no time-critical work (only MCI setup during the
  // ~2s loop re-issue). Drop it below normal so the ant threads and
  // main UI get scheduled first during any brief contention - the
  // tens-of-ms audio-setup window is where visible stutters came from.
  SetThreadPriority(s_bgmWorker, THREAD_PRIORITY_BELOW_NORMAL);
  // Block until the worker has registered its class and created the
  // hidden window (or failed trying). Past this point, commands posted
  // via PostBgmSync are guaranteed to have a valid s_bgmHwnd to use.
  WaitForSingleObject(s_bgmInitEvent, INFINITE);
  if (!s_bgmInitOk) {
    LOG(ERROR) << L"BGM worker init failed (hidden notify window setup)";
    WaitForSingleObject(s_bgmWorker, INFINITE);
    CloseHandle(s_bgmWorker);
    s_bgmWorker = nullptr;
    CloseHandle(s_bgmCmdEvent);
    s_bgmCmdEvent = nullptr;
    CloseHandle(s_bgmDoneEvent);
    s_bgmDoneEvent = nullptr;
    CloseHandle(s_bgmExitEvent);
    s_bgmExitEvent = nullptr;
    CloseHandle(s_bgmInitEvent);
    s_bgmInitEvent = nullptr;
    DeleteCriticalSection(&s_bgmCS);
    return false;
  }
  s_bgmInit = true;
  return ok;
}

void ShutdownBgm() {
  if (!s_bgmInit) {
    return;
  }
  // Signal exit and join. If the worker is currently mid-mciSendString
  // it'll finish that command first, then loop back, see the exit event,
  // destroy the hidden window and return - so "join" may wait up to one
  // play-setup duration (tens of ms). Acceptable at shutdown.
  SetEvent(s_bgmExitEvent);
  WaitForSingleObject(s_bgmWorker, INFINITE);
  CloseHandle(s_bgmWorker);
  s_bgmWorker = nullptr;
  CloseHandle(s_bgmCmdEvent);
  s_bgmCmdEvent = nullptr;
  CloseHandle(s_bgmDoneEvent);
  s_bgmDoneEvent = nullptr;
  CloseHandle(s_bgmExitEvent);
  s_bgmExitEvent = nullptr;
  CloseHandle(s_bgmInitEvent);
  s_bgmInitEvent = nullptr;
  DeleteCriticalSection(&s_bgmCS);
  s_bgmInit = false;
}
