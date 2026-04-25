#include "sound.h"

#include "globals.h"
#include "resource.h"
#include "utils.h"   // GetExeDir for the non-embedded file-source path

#include <logging.h>

volatile bool g_playsound = false;

// ==========================================================================
// BGM is driven by a dedicated worker thread that owns the MCI device
// end-to-end.
//
// Why a worker thread at all: mciSendStringW("play") blocks its caller
// for ~10-50ms while the waveform driver stages buffers and starts the
// mixer stream. When that call lived on the main thread (inside the
// MM_MCINOTIFY loop-back handler) every loop iteration visibly froze
// the ants canvas — especially at hyper/realtime speeds where a frame
// is 10 ms or less. Moving it to a worker keeps the UI smooth.
//
// Why the worker owns open + every other MCI op (not just the replay):
// some MCI drivers are thread-affinity-bound — a command sent from a
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
// own message loop — main thread is never in the loop path at all.
//
// Main thread talks to the worker through a single-slot command queue
// protected by s_bgmCS. User-triggered ops (Play / Pause / Stop) are
// sync — main posts the command, signals s_bgmCmdEvent, and waits on
// s_bgmDoneEvent. The loop replay needs no main-thread involvement, so
// there is no async post path.
// ==========================================================================

// ---------- Command queue (accessed from both threads under s_bgmCS) ------

enum class BgmCmd {
  None,
  Play,   // open if device is closed; resume if it's paused
  Pause,
  Stop,   // stop + close + temp-file cleanup
};

struct BgmCmdSlot {
  BgmCmd       cmd          = BgmCmd::None;
  std::wstring wav;
  bool         use_embedded = false;
};

static CRITICAL_SECTION s_bgmCS;
static BgmCmdSlot       s_slot;
static bool             s_lastResult    = false;
static HANDLE           s_bgmCmdEvent   = nullptr; // auto-reset: "cmd pending"
static HANDLE           s_bgmDoneEvent  = nullptr; // auto-reset: "sync cmd done"
static HANDLE           s_bgmExitEvent  = nullptr; // manual-reset: "shut down"
static HANDLE           s_bgmInitEvent  = nullptr; // manual-reset: "worker ready"
static HANDLE           s_bgmWorker     = nullptr;
static bool             s_bgmInit       = false;
static bool             s_bgmInitOk     = false;   // worker setup succeeded

// ---------- Worker-thread-only MCI state ----------------------------------
// Touched only from BgmWorkerProc; no synchronization needed.

static HWND         s_bgmHwnd          = nullptr; // hidden notify target
static bool         s_mciOpened        = false;
static std::wstring s_embeddedTempPath;
static const wchar_t kMciBgmAlias[]    = L"langton_ants_bgm";
static const wchar_t kBgmHiddenClass[] = L"LangtonAntsBgmHidden";

// Convert an MCIERROR code into a human-readable string using
// mciGetErrorStringW, the MCI-provided lookup. Used only for logging.
static std::wstring MciErrText(MCIERROR err) {
  wchar_t buf[256] = {};
  mciGetErrorStringW(err, buf, sizeof(buf) / sizeof(buf[0]));
  return std::wstring(buf);
}

// Materializes the IDR_BGM_WAVE resource to a file in the user's temp
// directory and returns that path (empty on failure). MCI's string API can
// only open named files — there is no "play from memory" form — so when
// we want to play the embedded WAV we have to stage it on disk first.
// Windows' FindResource returns a pointer to the PE-loaded resource bytes
// without extra allocation; we just WriteFile them out.
static std::wstring ExtractEmbeddedWavToTemp() {
  HRSRC hRsrc = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_BGM_WAVE), L"WAVE");
  if (hRsrc == nullptr) {
    LOG(ERROR) << L"FindResource IDR_BGM_WAVE failed";
    return std::wstring();
  }
  HGLOBAL hGlob = LoadResource(nullptr, hRsrc);
  if (hGlob == nullptr) {
    LOG(ERROR) << L"LoadResource IDR_BGM_WAVE failed";
    return std::wstring();
  }
  const LPVOID pData = LockResource(hGlob);
  const DWORD size = SizeofResource(nullptr, hRsrc);
  if (pData == nullptr || size == 0) {
    LOG(ERROR) << L"LockResource/SizeofResource returned empty for IDR_BGM_WAVE";
    return std::wstring();
  }

  wchar_t tempDir[MAX_PATH] = {};
  DWORD n = GetTempPathW(MAX_PATH, tempDir);
  if (n == 0 || n > MAX_PATH) {
    LOG(ERROR) << L"GetTempPathW failed";
    return std::wstring();
  }
  std::wstring tempPath = std::wstring(tempDir) + L"langton_ants_bgm.wav";

  HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << L"CreateFile temp failed for " << tempPath;
    return std::wstring();
  }
  DWORD written = 0;
  BOOL ok = WriteFile(hFile, pData, size, &written, nullptr);
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
    if (file.empty()) return false;
    s_embeddedTempPath = file;
  } else {
    const std::wstring cwd = GetExeDir();
    if (cwd.empty()) return false;
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
    g_playsound = false;
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
    g_playsound = false;
    return false;
  }
  g_playsound = true;
  return true;
}

static bool WorkerResume() {
  if (!s_mciOpened) return false;
  // `resume` continues from the current playback position. Do NOT re-issue
  // `play` here — that would reset to 0 and double-register the notify
  // callback.
  MCIERROR err = mciSendStringW(L"resume langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI resume failed: " << MciErrText(err);
    g_playsound = false;
    return false;
  }
  g_playsound = true;
  return true;
}

static bool WorkerPause() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // `pause` suspends playback without resetting position — the pending
  // `notify` registration stays alive so MM_MCINOTIFY still fires on the
  // eventual natural completion after a subsequent resume.
  MCIERROR err = mciSendStringW(L"pause langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) LOG(ERROR) << L"MCI pause failed: " << MciErrText(err);
  g_playsound = false;
  return true;
}

static bool WorkerStop() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // Full tear-down: stop halts playback, close releases the waveform
  // device so a subsequent open can succeed cleanly.
  MCIERROR err = mciSendStringW(L"stop langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) LOG(ERROR) << L"MCI stop failed: " << MciErrText(err);
  err = mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) LOG(ERROR) << L"MCI close failed: " << MciErrText(err);
  s_mciOpened = false;
  g_playsound = false;
  if (!s_embeddedTempPath.empty()) {
    DeleteFileW(s_embeddedTempPath.c_str());
    s_embeddedTempPath.clear();
  }
  return true;
}

// Loop re-issue. Called from BgmHiddenWndProc when MCI fires a successful
// MM_MCINOTIFY for the prior play. `from 0` is REQUIRED — after natural
// completion MCI leaves the cursor at EOF, so a bare `play` would be a
// silent no-op. The `notify` param re-registers s_bgmHwnd so the NEXT
// completion also lands in our message loop.
static void WorkerReplay() {
  if (!s_mciOpened || !g_playsound) return;
  MCIERROR err = mciSendStringW(L"play langton_ants_bgm from 0 notify",
                                 nullptr, 0, s_bgmHwnd);
  if (err != 0) {
    LOG(ERROR) << L"MCI replay failed: " << MciErrText(err);
  }
}

// Dispatch for BgmCmd values. Runs on the worker thread.
static bool ProcessCmd(const BgmCmdSlot& slot) {
  switch (slot.cmd) {
    case BgmCmd::Play:
      return s_mciOpened ? WorkerResume()
                          : WorkerOpenPlay(slot.wav, slot.use_embedded);
    case BgmCmd::Pause: return WorkerPause();
    case BgmCmd::Stop:  return WorkerStop();
    default:            return false;
  }
}

// Hidden window procedure. Runs on the worker thread when PeekMessage /
// DispatchMessage below fires MM_MCINOTIFY that MCI posted to s_bgmHwnd.
//
// wParam values we might see: MCI_NOTIFY_SUCCESSFUL (natural end-of-clip
// — we loop), MCI_NOTIFY_SUPERSEDED (another play took over — do nothing),
// MCI_NOTIFY_ABORTED (stop/close happened — do nothing), MCI_NOTIFY_FAILURE
// (driver error — do nothing). The g_playsound guard in WorkerReplay also
// protects against a late-arriving SUCCESSFUL notification that races a
// user Pause.
static LRESULT CALLBACK BgmHiddenWndProc(HWND hWnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
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
  WNDCLASSW wc = {};
  wc.lpfnWndProc   = BgmHiddenWndProc;
  wc.hInstance     = GetModuleHandleW(nullptr);
  wc.lpszClassName = kBgmHiddenClass;
  ATOM classAtom = RegisterClassW(&wc);
  if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LOG(ERROR) << L"BGM RegisterClass failed";
    s_bgmInitOk = false;
    SetEvent(s_bgmInitEvent);
    return 1;
  }
  // HWND_MESSAGE parent makes it an invisible message-only window — no
  // z-order, no taskbar, no focus changes. Pure PostMessage target.
  s_bgmHwnd = CreateWindowW(kBgmHiddenClass, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (s_bgmHwnd == nullptr) {
    LOG(ERROR) << L"BGM CreateWindow failed";
    UnregisterClassW(kBgmHiddenClass, wc.hInstance);
    s_bgmInitOk = false;
    SetEvent(s_bgmInitEvent);
    return 1;
  }
  s_bgmInitOk = true;
  SetEvent(s_bgmInitEvent);

  HANDLE handles[2] = { s_bgmExitEvent, s_bgmCmdEvent };
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
      // Window-message queue has something — dispatch everything pending.
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
static bool PostBgmSync(BgmCmd cmd, const std::wstring& wav = std::wstring(),
                        bool use_embedded = false) {
  if (!s_bgmInit) return false;
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
  return PostBgmSync(BgmCmd::Stop);
}

// Set true by AntPauseBgm when it actually paused the BGM. Cleared by
// AntResumeBgm after resuming, or by ToggleSound on any explicit user
// toggle — so an explicit mute/unmute during an ant pause "wins" over
// the auto-resume that would otherwise fire when ants un-pause. Main
// thread only.
static bool s_bgm_paused_by_ants = false;

bool ToggleSound() {
  // User is taking explicit control of sound; forget that we had auto-
  // paused it so we don't later contradict their choice on ant resume.
  s_bgm_paused_by_ants = false;
  if (g_playsound) {
    return PauseWavFile();
  } else {
    return PlayWavFile(sound_file, kUseEmbeddedBgm);
  }
}

void AntPauseBgm() {
  if (g_playsound) {
    PauseWavFile();
    s_bgm_paused_by_ants = true;
  }
}

void AntResumeBgm() {
  if (s_bgm_paused_by_ants) {
    s_bgm_paused_by_ants = false;
    PlayWavFile(sound_file, kUseEmbeddedBgm);
  }
}

// ---------- Lifecycle -----------------------------------------------------

void InitBgm() {
  if (s_bgmInit) return;
  InitializeCriticalSection(&s_bgmCS);
  s_bgmCmdEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
  s_bgmDoneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
  s_bgmExitEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr); // manual-reset
  s_bgmInitEvent = CreateEventW(nullptr, TRUE,  FALSE, nullptr); // manual-reset
  if (!s_bgmCmdEvent || !s_bgmDoneEvent || !s_bgmExitEvent || !s_bgmInitEvent) {
    LOG(ERROR) << L"BGM CreateEvent failed";
    return;
  }
  s_bgmWorker = CreateThread(nullptr, 0, BgmWorkerProc, nullptr, 0, nullptr);
  if (!s_bgmWorker) {
    LOG(ERROR) << L"BGM CreateThread failed";
    return;
  }
  // The worker does no time-critical work (only MCI setup during the
  // ~2s loop re-issue). Drop it below normal so the ant threads and
  // main UI get scheduled first during any brief contention — the
  // tens-of-ms audio-setup window is where visible stutters came from.
  SetThreadPriority(s_bgmWorker, THREAD_PRIORITY_BELOW_NORMAL);
  // Block until the worker has registered its class and created the
  // hidden window (or failed trying). Past this point, commands posted
  // via PostBgmSync are guaranteed to have a valid s_bgmHwnd to use.
  WaitForSingleObject(s_bgmInitEvent, INFINITE);
  if (!s_bgmInitOk) {
    LOG(ERROR) << L"BGM worker init failed";
    WaitForSingleObject(s_bgmWorker, INFINITE);
    CloseHandle(s_bgmWorker);    s_bgmWorker    = nullptr;
    CloseHandle(s_bgmCmdEvent);  s_bgmCmdEvent  = nullptr;
    CloseHandle(s_bgmDoneEvent); s_bgmDoneEvent = nullptr;
    CloseHandle(s_bgmExitEvent); s_bgmExitEvent = nullptr;
    CloseHandle(s_bgmInitEvent); s_bgmInitEvent = nullptr;
    DeleteCriticalSection(&s_bgmCS);
    return;
  }
  s_bgmInit = true;
}

void ShutdownBgm() {
  if (!s_bgmInit) return;
  // Signal exit and join. If the worker is currently mid-mciSendString
  // it'll finish that command first, then loop back, see the exit event,
  // destroy the hidden window and return — so "join" may wait up to one
  // play-setup duration (tens of ms). Acceptable at shutdown.
  SetEvent(s_bgmExitEvent);
  WaitForSingleObject(s_bgmWorker, INFINITE);
  CloseHandle(s_bgmWorker);    s_bgmWorker    = nullptr;
  CloseHandle(s_bgmCmdEvent);  s_bgmCmdEvent  = nullptr;
  CloseHandle(s_bgmDoneEvent); s_bgmDoneEvent = nullptr;
  CloseHandle(s_bgmExitEvent); s_bgmExitEvent = nullptr;
  CloseHandle(s_bgmInitEvent); s_bgmInitEvent = nullptr;
  DeleteCriticalSection(&s_bgmCS);
  s_bgmInit = false;
}
