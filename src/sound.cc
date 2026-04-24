#include "sound.h"

#include "globals.h"
#include "resource.h"
#include "utils.h"   // GetExeDir for the non-embedded file-source path

#include <logging.h>

volatile bool g_playsound = false;

// --- Background music via MCI ---------------------------------------------
//
// We drive background playback through MCI (Media Control Interface) rather
// than the simpler PlaySound API. Reason: Windows 2000's PlaySound cannot
// decode non-PCM WAV files reliably — a PlaySoundW call with an MS ADPCM
// file (the compact codec we use for ants.wav to keep the shipped file
// small) returns TRUE but silently drops the audio. Win2K's sndrec32 plays
// the exact same file fine; sndrec32 goes through MCI, and MCI's ACM
// interaction is the one that actually works cross-version.
//
// MCI takes plain ASCII-style command strings like `open <path> alias <name>`
// or `play <name> repeat`. mciSendStringW parses these and dispatches them
// to the appropriate MCI device driver (the waveform driver, in our case).
// Each command returns an MCIERROR (0 == success). A process-global alias
// ("langton_ants_bgm") names our open device instance so stop/close can find
// it again later.
//
// Using MCI also makes a future pause/resume feature trivial: the waveform
// driver supports `pause <alias>` and `resume <alias>` natively, preserving
// playback position — something PlaySound cannot do.

static const wchar_t kMciBgmAlias[] = L"langton_ants_bgm";
// Whether we currently hold the waveform device open under kMciBgmAlias.
// Avoids issuing commands against a non-existent alias (which MCI would
// answer with a confusing "device not open" error).
static bool s_mciOpened = false;
// Path of the temp .wav we materialized from IDR_BGM_WAVE (empty when the
// current session is playing from a side-by-side file instead). Populated
// on the first embedded-source PlayWavFile call; StopPlayWav removes the
// file from disk so we don't leak temp files across runs.
static std::wstring s_embeddedTempPath;

// Small helper: convert an MCIERROR code into a human-readable string using
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

  // GetTempPathW always returns a path with a trailing backslash, so we can
  // concatenate without an explicit separator.
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

bool PlayWavFile(const std::wstring& wav_file, bool use_embedded) {
  // File-source path still requires a filename; embedded source ignores it.
  if (!use_embedded && wav_file.empty()) {
    return false;
  }
  // If the device is already open, this call is a "resume from paused"
  // rather than a fresh play. Issue MCI `resume` to continue from the
  // current playback position — do NOT re-open (that would reset position
  // to 0) and do NOT re-issue `play` (the original play's notify is still
  // pending for the looping handler, and a fresh `play` would start over).
  if (s_mciOpened) {
    MCIERROR err = mciSendStringW(L"resume langton_ants_bgm", nullptr, 0, nullptr);
    if (err != 0) {
      LOG(ERROR) << L"MCI resume failed: " << MciErrText(err);
      g_playsound = false;
      return false;
    }
    g_playsound = true;
    return true;
  }

  // First-time open + play from position 0. `file` is the path MCI opens;
  // either the user-supplied name resolved against the exe dir, or the
  // temp file we materialize from the embedded IDR_BGM_WAVE resource.
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
    g_playsound = false;
    return false;
  }
  s_mciOpened = true;

  // `notify` asks the waveform driver to send MM_MCINOTIFY when playback
  // completes. mciSendStringW's FOURTH parameter is the callback HWND for
  // exactly this purpose — passing nullptr makes MCI silently drop the
  // notification, breaking the loop. We use mainHwnd so the notification
  // reaches our WindowProc's MM_MCINOTIFY handler.
  //
  // MCIWAVE's `play` does NOT support a `repeat` parameter (unlike MCIAVI
  // and MCICDA), so looping is done manually: on every successful completion
  // the handler re-issues `play ... from 0 notify` against the same callback.
  err = mciSendStringW(L"play langton_ants_bgm notify", nullptr, 0, mainHwnd);
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

bool PauseWavFile() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // `pause` suspends playback without resetting the position cursor — the
  // pending `notify` registration from the prior `play ... notify` stays
  // alive so that once PlayWavFile-as-resume is called and the clip finally
  // reaches its end, MM_MCINOTIFY still fires and the looping handler kicks
  // in. Critically we do NOT send `stop` or `close` here; those would drop
  // the position and the device instance respectively.
  MCIERROR err = mciSendStringW(L"pause langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI pause failed: " << MciErrText(err);
  }
  g_playsound = false;
  return true;
}

bool StopPlayWav() {
  if (!s_mciOpened) {
    g_playsound = false;
    return true;
  }
  // Full tear-down path (used by ShutDownApp). Stop halts playback; close
  // releases the waveform device so the next open can succeed cleanly (e.g.
  // if the app is re-launched within the same user session). The mute
  // toggle does NOT come through here — it uses PauseWavFile instead so
  // that the next unmute resumes from the pause point rather than
  // restarting from 0.
  MCIERROR err = mciSendStringW(L"stop langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI stop failed: " << MciErrText(err);
  }
  err = mciSendStringW(L"close langton_ants_bgm", nullptr, 0, nullptr);
  if (err != 0) {
    LOG(ERROR) << L"MCI close failed: " << MciErrText(err);
  }
  s_mciOpened = false;
  g_playsound = false;
  // If this session materialized the embedded WAV to a temp file, delete
  // it now so we don't litter %TEMP% with a stale copy across app runs.
  if (!s_embeddedTempPath.empty()) {
    DeleteFileW(s_embeddedTempPath.c_str());
    s_embeddedTempPath.clear();
  }
  return true;
}

// Set true by AntPauseBgm when it actually paused the BGM. Cleared by
// AntResumeBgm after resuming, or by ToggleSound on any explicit user
// toggle — so an explicit mute/unmute during an ant pause "wins" over
// the auto-resume that would otherwise fire when ants un-pause.
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
