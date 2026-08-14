#include "MirvPovReferenceHooks.h"

#include "MirvPovReferenceCompat.h"
#include "MirvPovReferenceCompat.h"
#include "MirvPovReferenceCompat.h"
#include "MirvPovReferenceIdentity.h"
#include "MirvPovReferencePov.h"
#include "MirvPovReferenceOffsets.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace live_hud {
namespace {

std::atomic<bool> g_installed{false};
std::atomic<bool> g_stop_watcher{false};
std::atomic<bool> g_demo_skipping_cached{false};
// 0=pending/not checked, 1=matching pinned client, -1=mismatch.
std::atomic<int> g_client_build_state{0};
HANDLE g_watcher_thread = nullptr;
std::uint8_t g_saved_je[sizeof(offsets::kEngine2FilterIsHltvJeBytes)]{};
bool g_je_patched = false;
void* g_je_address = nullptr;
std::atomic<std::uint64_t> g_tick{0};
std::atomic<std::uint64_t> g_filter_entries{0};
PVOID g_crash_veh = nullptr;
wchar_t g_crash_log_path[32768]{};
std::atomic<std::uint32_t> g_crash_probe_seq{0};
std::atomic<std::uintptr_t> g_crash_client_base{0};
std::uintptr_t g_crash_engine_base = 0;
std::uintptr_t g_crash_self_base = 0;

LONG CALLBACK crash_probe_veh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  const DWORD code = rec->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374u:  // heap corruption
    case 0xC0000409u:  // fail-fast / stack cookie
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const std::uint32_t seq =
      g_crash_probe_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  if (seq > 64 || g_crash_log_path[0] == L'\0') {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const auto ip = reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress);
  const auto client = g_crash_client_base.load(std::memory_order_relaxed);
  const char* module = "other";
  std::uintptr_t rva = ip;
  if (client && ip >= client &&
      ip < client + offsets::kExpectedClientSize) {
    module = "client";
    rva = ip - client;
  } else if (g_crash_engine_base && ip >= g_crash_engine_base &&
             ip < g_crash_engine_base + offsets::kExpectedEngine2Size) {
    module = "engine2";
    rva = ip - g_crash_engine_base;
  } else if (g_crash_self_base && ip >= g_crash_self_base &&
             ip < g_crash_self_base + 0x200000) {
    module = "live_hud";
    rva = ip - g_crash_self_base;
  }

  unsigned long long operation = ~0ull;
  unsigned long long fault = 0;
  if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    operation = static_cast<unsigned long long>(rec->ExceptionInformation[0]);
    fault = static_cast<unsigned long long>(rec->ExceptionInformation[1]);
  }
  unsigned long long rax = 0;
  unsigned long long rcx = 0;
  unsigned long long rdx = 0;
  unsigned long long rsp = 0;
#if defined(_M_X64)
  if (info->ContextRecord) {
    rax = info->ContextRecord->Rax;
    rcx = info->ContextRecord->Rcx;
    rdx = info->ContextRecord->Rdx;
    rsp = info->ContextRecord->Rsp;
  }
#endif
  char line[640]{};
  const int len = std::snprintf(
      line, sizeof(line),
      "crash_probe=seq=%u code=0x%08lX ip=0x%llX module=%s rva=0x%llX "
      "op=%llu fault=0x%llX flags=0x%08lX rax=0x%llX rcx=0x%llX "
      "rdx=0x%llX rsp=0x%llX tid=%lu\r\n",
      seq, static_cast<unsigned long>(code),
      static_cast<unsigned long long>(ip), module,
      static_cast<unsigned long long>(rva), operation, fault,
      static_cast<unsigned long>(rec->ExceptionFlags), rax, rcx, rdx, rsp,
      static_cast<unsigned long>(GetCurrentThreadId()));
  if (len > 0) {
    HANDLE file = CreateFileW(g_crash_log_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::uint8_t g_last_ishtlv = 0xFF;
std::uint8_t g_last_goto_blocked = 0xFF;
std::uint8_t g_last_playing = 0xFF;
std::int32_t g_last_skip = 0x7FFFFFFF;

using FilterFn = std::int64_t(__fastcall*)(void*, void*, void*, void*);
FilterFn g_filter_trampoline = nullptr;
std::uint8_t g_filter_stolen[16]{};
std::uint8_t* g_filter_entry = nullptr;
bool g_filter_hooked = false;

// Radar-only experiment: patch two IsHLTVOrReplay call sites in HudRadar.
std::uint8_t* g_radar_sites[2]{};
std::uint8_t g_radar_saved[2][sizeof(offsets::kClientForceAlZero)]{};
bool g_radar_patched = false;
bool g_radar_pending_logged = false;
bool g_radar_finished = false;  // success or permanent fail after client seen

// Whole-function lie (broken camera) vs filtered trampoline (radar allowlist).
std::uint8_t* g_ishtlv_lie_addr = nullptr;
std::uint8_t g_ishtlv_lie_saved[sizeof(offsets::kEngine2IsHltvOrReplayLie)]{};
bool g_ishtlv_lie_all_patched = false;

using IsHltvFn = bool(__fastcall*)(void*);
IsHltvFn g_ishtlv_trampoline = nullptr;
std::uint8_t* g_ishtlv_tramp_mem = nullptr;
std::uint8_t* g_ishtlv_entry = nullptr;
std::uint8_t g_ishtlv_stolen[16]{};
bool g_ishtlv_hooked = false;
// 0=filtered allowlist, 1=pipeline (lie default, honest for camera)
int g_ishtlv_lie_style = 0;

// C_HLTVCamera methods live around client RVA 0xB44000..0xB52000 (vtable survey).
constexpr std::uint32_t kClientHltvCameraCodeLo = 0xB44000;
constexpr std::uint32_t kClientHltvCameraCodeHi = 0xB52000;

struct AddrRange {
  std::uintptr_t begin = 0;
  std::uintptr_t end = 0;
};
constexpr int kMaxAllowRanges = 96;
AddrRange g_radar_allow[kMaxAllowRanges]{};
int g_radar_allow_n = 0;
bool g_radar_allow_ready = false;
bool g_radar_allow_pending_logged = false;
bool g_radar_allow_done = false;  // success or permanent fail after client seen
// Broad HudRadar window for identity stack filtering (not the IsHLTV site list).
constexpr int kMaxHudWindows = 12;
AddrRange g_hud_windows[kMaxHudWindows]{};
int g_hud_window_n = 0;
bool g_hud_identity_ready = false;

void hud_window_add(std::uintptr_t lo, std::uintptr_t hi) {
  if (lo >= hi || g_hud_window_n >= kMaxHudWindows) {
    return;
  }
  g_hud_windows[g_hud_window_n++] = AddrRange{lo, hi};
}

bool addr_in_hud_windows(std::uintptr_t addr) {
  for (int i = 0; i < g_hud_window_n; ++i) {
    if (addr >= g_hud_windows[i].begin && addr < g_hud_windows[i].end) {
      return true;
    }
  }
  return false;
}

PeFingerprint fingerprint_loaded_module(HMODULE mod) {
  PeFingerprint fp{};
  if (!mod) {
    return fp;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return fp;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return fp;
  }
  fp.size_of_image = nt->OptionalHeader.SizeOfImage;
  fp.time_date_stamp = nt->FileHeader.TimeDateStamp;
  return fp;
}

void install_crash_probe(HMODULE engine2) {
  if (g_crash_veh) {
    return;
  }
  const std::wstring path = temp_log_path().wstring();
  wcsncpy_s(g_crash_log_path, path.c_str(), _TRUNCATE);
  g_crash_engine_base = reinterpret_cast<std::uintptr_t>(engine2);
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<const void*>(&crash_probe_veh), &mbi,
                   sizeof(mbi)) == sizeof(mbi)) {
    g_crash_self_base = reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
  }
  g_crash_veh = AddVectoredExceptionHandler(1, &crash_probe_veh);
  char detail[192]{};
  std::snprintf(detail, sizeof(detail),
                "installed=%d engine2=0x%llX live_hud=0x%llX",
                g_crash_veh ? 1 : 0,
                static_cast<unsigned long long>(g_crash_engine_base),
                static_cast<unsigned long long>(g_crash_self_base));
  log_kv("crash_probe", detail);
}

void remove_crash_probe() {
  if (g_crash_veh) {
    RemoveVectoredExceptionHandler(g_crash_veh);
    g_crash_veh = nullptr;
  }
}

bool env_flag_one(const char* name) {
  char buf[8]{};
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && buf[0] == '1' && buf[1] == '\0';
}

bool want_clear_is_hltv() { return env_flag_one("LIVE_HUD_CLEAR_IS_HLTV"); }
bool want_nop_filter() { return env_flag_one("LIVE_HUD_NOP_FILTER"); }
bool want_radar_live() { return env_flag_one("LIVE_HUD_RADAR_LIVE"); }
// Caller-filtered IsHLTVOrReplay lie (HudRadar allowlist only).
bool want_ishtlv_lie() { return env_flag_one("LIVE_HUD_ISHLTV_LIE"); }
// Nuclear: force all callers false (OOB without a follow pawn — research only).
bool want_ishtlv_lie_all() { return env_flag_one("LIVE_HUD_ISHLTV_LIE_ALL"); }
// PIPELINE uses its own lie: default false, but honest for C_HLTVCamera/view.
bool want_count_filter() {
  // Opt-in only: trampoline is unused during HLTV demo playback (counter
  // freezes after ishltv=1) and has crashed sessions near demo end.
  return env_flag_one("LIVE_HUD_COUNT_FILTER");
}

void* clientstate_ptr(HMODULE engine2) {
  auto** slot = reinterpret_cast<void**>(
      reinterpret_cast<std::uint8_t*>(engine2) +
      offsets::kEngine2ClientStatePtrRva);
  return *slot;
}

std::uint8_t* demo_player(HMODULE engine2) {
  return reinterpret_cast<std::uint8_t*>(engine2) +
         offsets::kEngine2DemoPlayerObjRva;
}

bool patch_filter_je(HMODULE engine2) {
  auto* addr = reinterpret_cast<std::uint8_t*>(engine2) +
               offsets::kEngine2FilterIsHltvJeRva;
  if (std::memcmp(addr, offsets::kEngine2FilterIsHltvJeBytes,
                  sizeof(offsets::kEngine2FilterIsHltvJeBytes)) != 0) {
    log_kv("net_gate", "filter_je_bytes_mismatch");
    return false;
  }

  DWORD old_prot = 0;
  if (!VirtualProtect(addr, sizeof(offsets::kEngine2FilterIsHltvJeBytes),
                      PAGE_EXECUTE_READWRITE, &old_prot)) {
    log_kv("net_gate", "virtualprotect_failed");
    return false;
  }

  std::memcpy(g_saved_je, addr, sizeof(g_saved_je));
  std::memset(addr, 0x90, sizeof(offsets::kEngine2FilterIsHltvJeBytes));
  FlushInstructionCache(GetCurrentProcess(), addr,
                        sizeof(offsets::kEngine2FilterIsHltvJeBytes));
  VirtualProtect(addr, sizeof(offsets::kEngine2FilterIsHltvJeBytes), old_prot,
                 &old_prot);

  g_je_address = addr;
  g_je_patched = true;
  return true;
}

void restore_filter_je() {
  if (!g_je_patched || !g_je_address) {
    return;
  }
  DWORD old_prot = 0;
  if (VirtualProtect(g_je_address, sizeof(g_saved_je), PAGE_EXECUTE_READWRITE,
                     &old_prot)) {
    std::memcpy(g_je_address, g_saved_je, sizeof(g_saved_je));
    FlushInstructionCache(GetCurrentProcess(), g_je_address, sizeof(g_saved_je));
    VirtualProtect(g_je_address, sizeof(g_saved_je), old_prot, &old_prot);
  }
  g_je_patched = false;
  g_je_address = nullptr;
}

bool patch_ishtlv_lie_all(HMODULE engine2) {
  if (g_ishtlv_lie_all_patched) {
    return true;
  }
  auto* addr = reinterpret_cast<std::uint8_t*>(engine2) +
               offsets::kEngine2IsHltvOrReplayRva;
  if (std::memcmp(addr, offsets::kEngine2IsHltvOrReplayPrologue,
                  sizeof(offsets::kEngine2IsHltvOrReplayPrologue)) != 0) {
    log_kv("ishtlv_lie", "prologue_mismatch");
    return false;
  }

  DWORD old_prot = 0;
  constexpr std::size_t kLen = sizeof(offsets::kEngine2IsHltvOrReplayLie);
  if (!VirtualProtect(addr, kLen, PAGE_EXECUTE_READWRITE, &old_prot)) {
    log_kv("ishtlv_lie", "virtualprotect_failed");
    return false;
  }
  std::memcpy(g_ishtlv_lie_saved, addr, kLen);
  std::memcpy(addr, offsets::kEngine2IsHltvOrReplayLie, kLen);
  FlushInstructionCache(GetCurrentProcess(), addr, kLen);
  VirtualProtect(addr, kLen, old_prot, &old_prot);

  g_ishtlv_lie_addr = addr;
  g_ishtlv_lie_all_patched = true;
  log_kv("ishtlv_lie", "all_patched_UNSAFE");
  return true;
}

void restore_ishtlv_lie_all() {
  if (!g_ishtlv_lie_all_patched || !g_ishtlv_lie_addr) {
    return;
  }
  constexpr std::size_t kLen = sizeof(offsets::kEngine2IsHltvOrReplayLie);
  DWORD old_prot = 0;
  if (VirtualProtect(g_ishtlv_lie_addr, kLen, PAGE_EXECUTE_READWRITE,
                     &old_prot)) {
    std::memcpy(g_ishtlv_lie_addr, g_ishtlv_lie_saved, kLen);
    FlushInstructionCache(GetCurrentProcess(), g_ishtlv_lie_addr, kLen);
    VirtualProtect(g_ishtlv_lie_addr, kLen, old_prot, &old_prot);
  }
  g_ishtlv_lie_all_patched = false;
  g_ishtlv_lie_addr = nullptr;
}

std::uint32_t module_image_size(HMODULE mod) {
  return fingerprint_loaded_module(mod).size_of_image;
}

bool __fastcall ishltv_or_replay_hook(void* self);

// Minimal x64 absolute jump trampoline.
#pragma pack(push, 1)
struct AbsJump {
  std::uint8_t mov_rax[2]{0x48, 0xB8};
  std::uint64_t imm = 0;
  std::uint8_t jmp_rax[2]{0xFF, 0xE0};
};
#pragma pack(pop)

bool ret_in_radar_allowlist(void* ret) {
  const auto r = reinterpret_cast<std::uintptr_t>(ret);
  for (int i = 0; i < g_radar_allow_n; ++i) {
    if (r >= g_radar_allow[i].begin && r < g_radar_allow[i].end) {
      return true;
    }
  }
  return false;
}

bool ret_in_hltv_camera_code(void* ret) {
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(client);
  const auto r = reinterpret_cast<std::uintptr_t>(ret);
  return r >= base + kClientHltvCameraCodeLo &&
         r < base + kClientHltvCameraCodeHi;
}

void allow_add_range(std::uintptr_t begin, std::uintptr_t end) {
  if (begin >= end || g_radar_allow_n >= kMaxAllowRanges) {
    return;
  }
  for (int i = 0; i < g_radar_allow_n; ++i) {
    if (end < g_radar_allow[i].begin || begin > g_radar_allow[i].end) {
      continue;
    }
    if (begin < g_radar_allow[i].begin) {
      g_radar_allow[i].begin = begin;
    }
    if (end > g_radar_allow[i].end) {
      g_radar_allow[i].end = end;
    }
    return;
  }
  g_radar_allow[g_radar_allow_n++] = AddrRange{begin, end};
}

bool build_radar_allowlist(HMODULE client) {
  g_radar_allow_n = 0;
  g_hud_window_n = 0;
  g_hud_identity_ready = false;
  const auto* base = reinterpret_cast<const std::uint8_t*>(client);
  const std::uint32_t size = module_image_size(client);
  if (!base || size < 0x1000) {
    return false;
  }

  constexpr auto kPairPat = sizeof(offsets::kClientCallIsHltvOrReplay);
  const std::uint32_t last = size - static_cast<std::uint32_t>(kPairPat);
  std::uint32_t pair_a = 0;
  std::uint32_t pair_b = 0;
  int pairs = 0;
  for (std::uint32_t i = 0; i <= last; ++i) {
    if (std::memcmp(base + i, offsets::kClientCallIsHltvOrReplay, kPairPat) !=
        0) {
      continue;
    }
    const std::uint32_t j = i + offsets::kClientRadarCallPairGap;
    if (j > last) {
      continue;
    }
    if (std::memcmp(base + j, offsets::kClientCallIsHltvOrReplay, kPairPat) !=
        0) {
      continue;
    }
    ++pairs;
    pair_a = i;
    pair_b = j;
  }
  if (pairs != 1) {
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "allow_pairs=%d", pairs);
    log_kv("ishtlv_lie", msg);
    return false;
  }

  const std::uint32_t mid = pair_a + (pair_b - pair_a) / 2;
  // Wider than IsHLTV site scan: identity stack frames land in nearby HUD
  // helpers (weapon/health), not only on the +0x2B0 call bytes.
  const std::uint32_t id_radius = offsets::kClientRadarAllowRadius + 0x20000;
  const std::uint32_t id_lo = mid > id_radius ? mid - id_radius : 0;
  const std::uint32_t id_hi =
      mid + id_radius < size ? mid + id_radius : size - 1;
  hud_window_add(reinterpret_cast<std::uintptr_t>(base + id_lo),
                 reinterpret_cast<std::uintptr_t>(base + id_hi));
  // Do not add broad C0A000-C10000 / C80000-C90000 game-event windows.
  // round_end is replayed at seek start before skip_tick becomes observable;
  // a deep C8 frame made slot->pawn return the soon-to-be-destroyed follow
  // pawn, then C81720 dereferenced its missing controller at C821AF. Kill
  // feedback uses the direct C81E00 return-address exception in identity.cpp.
  if (size > 0x4F2000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0x4EA000),
                   reinterpret_cast<std::uintptr_t>(base + 0x4F2000));
  }
  // CCSGO_HudDeathNotice / player_death HUD feed (GetLocalPlayer* gates).
  if (size > 0xE10000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0xDF0000),
                   reinterpret_cast<std::uintptr_t>(base + 0xE10000));
  }
  // TeamCounter/HudMoney band (live identity consumers).
  if (size > 0xE4A000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0xE48000),
                   reinterpret_cast<std::uintptr_t>(base + 0xE4A000));
  }
  // Panorama "flashed"/"flashed_amount": CAC520 → c10d30 → GetLocalPlayerPawn
  // → CBFD10. Remap via CAC/CBF stack windows (not c10d30 alone — that
  // wrapper is shared; carpet-remapping it would poison demoui).
  if (size > 0xCAD000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0xCAC000),
                   reinterpret_cast<std::uintptr_t>(base + 0xCAD000));
  }
  if (size > 0xCC1000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0xCBF000),
                   reinterpret_cast<std::uintptr_t>(base + 0xCC1000));
  }
  // HudChat / RadioText / TextMsg / ChatPrintf (grenade + teammate notices).
  // Do NOT add 0xE35000–0xE36000 (PushNotice sits inside HudRadar/VoiceStatus
  // band — carpet IsHLTV/identity there collapses teammate radar/top strip).
  if (size > 0x1111000) {
    hud_window_add(reinterpret_cast<std::uintptr_t>(base + 0x110A000),
                   reinterpret_cast<std::uintptr_t>(base + 0x1111000));
  }
  g_hud_identity_ready = g_hud_window_n > 0;

  const std::uint32_t radius = offsets::kClientRadarAllowRadius;
  const std::uint32_t lo = mid > radius ? mid - radius : 0;
  const std::uint32_t hi =
      mid + radius < size ? mid + radius : size - 1;

  // Collect +0x2B0 call sites in the HudRadar band (layout + icon style).
  // Layout sites stay in the lie list: live top-strip (own team only) needs
  // IsHLTVOrReplay false there. Icon sites need the same lie AND !IsObserver
  // (see identity IsObserver hook) for live 5-color dots.
  constexpr auto kCallLen = sizeof(offsets::kClientCallVtable2B0);
  int sites = 0;
  for (std::uint32_t i = lo; i + kCallLen <= hi; ++i) {
    if (std::memcmp(base + i, offsets::kClientCallVtable2B0, kCallLen) != 0) {
      continue;
    }
    const auto abs = reinterpret_cast<std::uintptr_t>(base + i);
    allow_add_range(abs, abs + 32);
    ++sites;
  }
  // Ensure the UpdateSquareLayout pair (mov+call form) is covered even if the
  // bare call scanner drifts.
  allow_add_range(reinterpret_cast<std::uintptr_t>(base + pair_a),
                  reinterpret_cast<std::uintptr_t>(base + pair_b) + 32);

  char msg[96]{};
  std::snprintf(msg, sizeof(msg),
                "allow_sites=%d ranges=%d rva_mid=0x%X id_wins=%d", sites,
                g_radar_allow_n, mid, g_hud_window_n);
  log_kv("ishtlv_lie", msg);
  g_radar_allow_ready = true;
  return true;
}

bool stack_in_hud_identity_window() {
  if (!g_hud_identity_ready) {
    return false;
  }
  // Shallow walk only — full 32-frame CaptureStackBackTrace on every IsHLTV
  // call cost ~100 FPS. Icon number bits are handled by style/paint NOPs now.
  void* frames[8]{};
  const USHORT n = CaptureStackBackTrace(0, 8, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    if (addr_in_hud_windows(reinterpret_cast<std::uintptr_t>(frames[i]))) {
      return true;
    }
  }
  return false;
}

bool __fastcall ishltv_or_replay_hook(void* self) {
  // Pipeline V2 changes the mode predicate only inside an explicit native
  // HUD/message/render transaction. Camera/demo code outside that scope keeps
  // the honest HLTV result, so chase-camera state and net decoding are not
  // poisoned by a process-wide lie.
  if (want_pipeline()) {
    if (pov::active() && !demo_is_skipping()) {
      return false;
    }
    return g_ishtlv_trampoline(self);
  }

  void* ret = _ReturnAddress();
  // TeamCounter money reveal: even mid-seek, stay on live freezetime path so
  // demoui scrub does not force always-visible economy panels.
  if (ret && g_hud_identity_ready) {
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    if (client) {
      const auto rva = static_cast<std::uint32_t>(
          reinterpret_cast<std::uint8_t*>(ret) -
          reinterpret_cast<std::uint8_t*>(client));
      if (rva >= 0xE48000 && rva < 0xE4A000) {
        return false;
      }
    }
  }
  // Cheap checks only. Mid-seek used to stack-walk every IsHLTV (seek stutter).
  const bool allow =
      (g_radar_allow_ready && ret_in_radar_allowlist(ret)) ||
      (ret && addr_in_hud_windows(reinterpret_cast<std::uintptr_t>(ret)));
  if (allow) {
    return false;
  }
  return g_ishtlv_trampoline(self);
}

bool install_ishtlv_lie_hook(HMODULE engine2, int style) {
  if (g_ishtlv_hooked) {
    g_ishtlv_lie_style = style;
    return true;
  }
  g_ishtlv_lie_style = style;
  g_ishtlv_entry = reinterpret_cast<std::uint8_t*>(engine2) +
                   offsets::kEngine2IsHltvOrReplayRva;
  if (std::memcmp(g_ishtlv_entry, offsets::kEngine2IsHltvOrReplayPrologue,
                  sizeof(offsets::kEngine2IsHltvOrReplayPrologue)) != 0) {
    log_kv("ishtlv_lie", "prologue_mismatch");
    return false;
  }

  constexpr std::size_t kStolen = offsets::kEngine2IsHltvOrReplayStolen;
  static_assert(kStolen == 12);
  static_assert(sizeof(AbsJump) == 12);
  // Prologue: mov rax,[rip+disp] / test rax,rax / je fail.
  // Do NOT memcpy RIP-relative bytes into a far trampoline (same crash class
  // as the first identity hook). Rebuild with an absolute load of the
  // clientstate pointer slot, then abs-jump to continue/fail paths.
  std::memcpy(g_ishtlv_stolen, g_ishtlv_entry, kStolen);

  auto* tramp = reinterpret_cast<std::uint8_t*>(VirtualAlloc(
      nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) {
    log_kv("ishtlv_lie", "tramp_alloc_failed");
    return false;
  }
  g_ishtlv_tramp_mem = tramp;

  const auto* cs_slot = reinterpret_cast<std::uint8_t*>(engine2) +
                        offsets::kEngine2ClientStatePtrRva;
  // Continue at first instruction after the stolen je (entry+12).
  // Fail path is the original je target (entry+0x21).
  void* cont = g_ishtlv_entry + kStolen;
  void* fail = g_ishtlv_entry + 0x21;

  std::size_t o = 0;
  // mov rax, imm64  ; address of clientstate pointer qword
  tramp[o++] = 0x48;
  tramp[o++] = 0xB8;
  const auto slot_imm = reinterpret_cast<std::uint64_t>(cs_slot);
  std::memcpy(tramp + o, &slot_imm, sizeof(slot_imm));
  o += sizeof(slot_imm);
  // mov rax, [rax]
  tramp[o++] = 0x48;
  tramp[o++] = 0x8B;
  tramp[o++] = 0x00;
  // test rax, rax
  tramp[o++] = 0x48;
  tramp[o++] = 0x85;
  tramp[o++] = 0xC0;
  // jne +12  ; skip fail AbsJump when clientstate is non-null
  tramp[o++] = 0x75;
  tramp[o++] = 0x0C;
  AbsJump to_fail{};
  to_fail.imm = reinterpret_cast<std::uint64_t>(fail);
  std::memcpy(tramp + o, &to_fail, sizeof(to_fail));
  o += sizeof(to_fail);
  AbsJump to_cont{};
  to_cont.imm = reinterpret_cast<std::uint64_t>(cont);
  std::memcpy(tramp + o, &to_cont, sizeof(to_cont));
  o += sizeof(to_cont);
  FlushInstructionCache(GetCurrentProcess(), tramp, o);
  g_ishtlv_trampoline = reinterpret_cast<IsHltvFn>(tramp);

  DWORD old_prot = 0;
  if (!VirtualProtect(g_ishtlv_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    log_kv("ishtlv_lie", "virtualprotect_failed");
    VirtualFree(tramp, 0, MEM_RELEASE);
    g_ishtlv_tramp_mem = nullptr;
    g_ishtlv_trampoline = nullptr;
    return false;
  }
  AbsJump to_hook{};
  to_hook.imm = reinterpret_cast<std::uint64_t>(&ishltv_or_replay_hook);
  std::memcpy(g_ishtlv_entry, &to_hook, sizeof(to_hook));
  FlushInstructionCache(GetCurrentProcess(), g_ishtlv_entry, kStolen);
  VirtualProtect(g_ishtlv_entry, kStolen, old_prot, &old_prot);

  g_ishtlv_hooked = true;
  log_kv("ishtlv_lie",
         style == 1 ? "pipeline_camera_deny" : "filtered_hooked");
  return true;
}

bool install_ishtlv_lie_filtered(HMODULE engine2) {
  return install_ishtlv_lie_hook(engine2, 0);
}

bool install_ishtlv_lie_pipeline(HMODULE engine2) {
  return install_ishtlv_lie_hook(engine2, 1);
}

void restore_ishtlv_lie_filtered() {
  if (!g_ishtlv_hooked || !g_ishtlv_entry) {
    return;
  }
  constexpr std::size_t kStolen = offsets::kEngine2IsHltvOrReplayStolen;
  DWORD old_prot = 0;
  if (VirtualProtect(g_ishtlv_entry, kStolen, PAGE_EXECUTE_READWRITE,
                     &old_prot)) {
    std::memcpy(g_ishtlv_entry, g_ishtlv_stolen, kStolen);
    FlushInstructionCache(GetCurrentProcess(), g_ishtlv_entry, kStolen);
    VirtualProtect(g_ishtlv_entry, kStolen, old_prot, &old_prot);
  }
  g_ishtlv_hooked = false;
  g_ishtlv_entry = nullptr;
  g_ishtlv_trampoline = nullptr;
  if (g_ishtlv_tramp_mem) {
    VirtualFree(g_ishtlv_tramp_mem, 0, MEM_RELEASE);
    g_ishtlv_tramp_mem = nullptr;
  }
}

void restore_ishtlv_lie() {
  restore_ishtlv_lie_filtered();
  restore_ishtlv_lie_all();
}

void try_radar_allowlist_once() {
  // PIPELINE installs the same filtered IsHLTV hook; allowlist must build for
  // both LIVE_HUD_ISHLTV_LIE and LIVE_HUD_PIPELINE (was gated on lie-only →
  // allow=0 forever under pipeline, so radar/top stayed demo).
  if (!(want_ishtlv_lie() || want_pipeline()) || g_radar_allow_done ||
      g_ishtlv_lie_all_patched) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    if (!g_radar_allow_pending_logged) {
      log_kv("ishtlv_lie", "allow_client_pending");
      g_radar_allow_pending_logged = true;
    }
    return;
  }
  if (!client_build_matches()) {
    return;
  }
  if (!build_radar_allowlist(client)) {
    log_kv("ishtlv_lie", "allow_build_failed");
  }
  g_radar_allow_done = true;
}

std::int64_t __fastcall filter_hook(void* a, void* b, void* c, void* d) {
  g_filter_entries.fetch_add(1, std::memory_order_relaxed);
  return g_filter_trampoline(a, b, c, d);
}

bool install_filter_counter(HMODULE engine2) {
  g_filter_entry = reinterpret_cast<std::uint8_t*>(engine2) +
                   offsets::kEngine2FilterOrBufferRva;
  // Need 14 bytes for abs jump; Filter prologue is longer.
  constexpr std::size_t kStolen = 14;
  std::memcpy(g_filter_stolen, g_filter_entry, kStolen);

  auto* tramp = reinterpret_cast<std::uint8_t*>(VirtualAlloc(
      nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) {
    log_kv("filter_count", "tramp_alloc_failed");
    return false;
  }
  std::memcpy(tramp, g_filter_stolen, kStolen);
  AbsJump back{};
  back.imm = reinterpret_cast<std::uint64_t>(g_filter_entry + kStolen);
  std::memcpy(tramp + kStolen, &back, sizeof(back));
  g_filter_trampoline = reinterpret_cast<FilterFn>(tramp);

  DWORD old_prot = 0;
  if (!VirtualProtect(g_filter_entry, kStolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
    log_kv("filter_count", "protect_failed");
    return false;
  }
  AbsJump to_hook{};
  to_hook.imm = reinterpret_cast<std::uint64_t>(&filter_hook);
  std::memcpy(g_filter_entry, &to_hook, sizeof(to_hook));
  // Pad remaining stolen bytes with NOP if any (14-12=2).
  static_assert(sizeof(AbsJump) == 12);
  g_filter_entry[12] = 0x90;
  g_filter_entry[13] = 0x90;
  FlushInstructionCache(GetCurrentProcess(), g_filter_entry, kStolen);
  VirtualProtect(g_filter_entry, kStolen, old_prot, &old_prot);
  g_filter_hooked = true;
  log_kv("filter_count", "hooked");
  return true;
}

void restore_filter_counter() {
  if (!g_filter_hooked || !g_filter_entry) {
    return;
  }
  DWORD old_prot = 0;
  if (VirtualProtect(g_filter_entry, 14, PAGE_EXECUTE_READWRITE, &old_prot)) {
    std::memcpy(g_filter_entry, g_filter_stolen, 14);
    FlushInstructionCache(GetCurrentProcess(), g_filter_entry, 14);
    VirtualProtect(g_filter_entry, 14, old_prot, &old_prot);
  }
  g_filter_hooked = false;
}

void restore_radar_live();

// Find unique HudRadar pair: two IsHLTVOrReplay call prologues gap 0xFC apart.
bool find_radar_ishtlv_sites(HMODULE client, std::uint8_t** out_a,
                             std::uint8_t** out_b) {
  const auto* base = reinterpret_cast<const std::uint8_t*>(client);
  const std::uint32_t size = module_image_size(client);
  if (!base || size < sizeof(offsets::kClientCallIsHltvOrReplay) +
                           offsets::kClientRadarCallPairGap) {
    return false;
  }

  constexpr auto kPatLen = sizeof(offsets::kClientCallIsHltvOrReplay);
  const std::uint32_t last =
      size - static_cast<std::uint32_t>(kPatLen);
  std::uint8_t* first = nullptr;
  std::uint8_t* second = nullptr;
  int pair_count = 0;

  for (std::uint32_t i = 0; i <= last; ++i) {
    if (std::memcmp(base + i, offsets::kClientCallIsHltvOrReplay, kPatLen) !=
        0) {
      continue;
    }
    const std::uint32_t j = i + offsets::kClientRadarCallPairGap;
    if (j > last) {
      continue;
    }
    if (std::memcmp(base + j, offsets::kClientCallIsHltvOrReplay, kPatLen) !=
        0) {
      continue;
    }
    ++pair_count;
    first = const_cast<std::uint8_t*>(base + i);
    second = const_cast<std::uint8_t*>(base + j);
  }

  if (pair_count != 1 || !first || !second) {
    log_kv("radar_live",
           ("scan_pairs=" + std::to_string(pair_count)).c_str());
    return false;
  }
  *out_a = first;
  *out_b = second;
  return true;
}

bool patch_radar_live(HMODULE client) {
  if (g_radar_patched) {
    return true;
  }
  std::uint8_t* sites[2]{};
  if (!find_radar_ishtlv_sites(client, &sites[0], &sites[1])) {
    return false;
  }

  // Patch CALL only (bytes +3..+8), leave mov rax,[rcx].
  constexpr std::size_t kCallOff = 3;
  constexpr std::size_t kPatchLen = sizeof(offsets::kClientForceAlZero);

  for (int i = 0; i < 2; ++i) {
    auto* call = sites[i] + kCallOff;
    if (call[0] != 0xFF || call[1] != 0x90) {
      log_kv("radar_live", "call_bytes_mismatch");
      restore_radar_live();
      return false;
    }
    DWORD old_prot = 0;
    if (!VirtualProtect(call, kPatchLen, PAGE_EXECUTE_READWRITE, &old_prot)) {
      log_kv("radar_live", "virtualprotect_failed");
      restore_radar_live();
      return false;
    }
    std::memcpy(g_radar_saved[i], call, kPatchLen);
    std::memcpy(call, offsets::kClientForceAlZero, kPatchLen);
    FlushInstructionCache(GetCurrentProcess(), call, kPatchLen);
    VirtualProtect(call, kPatchLen, old_prot, &old_prot);
    g_radar_sites[i] = call;
  }

  g_radar_patched = true;
  const auto rva0 = static_cast<std::uint32_t>(
      g_radar_sites[0] - reinterpret_cast<std::uint8_t*>(client));
  const auto rva1 = static_cast<std::uint32_t>(
      g_radar_sites[1] - reinterpret_cast<std::uint8_t*>(client));
  char msg[96]{};
  std::snprintf(msg, sizeof(msg), "patched rva0=0x%X rva1=0x%X", rva0, rva1);
  log_kv("radar_live", msg);
  return true;
}

void restore_radar_live() {
  if (!g_radar_sites[0] && !g_radar_sites[1]) {
    g_radar_patched = false;
    return;
  }
  constexpr std::size_t kPatchLen = sizeof(offsets::kClientForceAlZero);
  for (int i = 0; i < 2; ++i) {
    if (!g_radar_sites[i]) {
      continue;
    }
    DWORD old_prot = 0;
    if (VirtualProtect(g_radar_sites[i], kPatchLen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_radar_sites[i], g_radar_saved[i], kPatchLen);
      FlushInstructionCache(GetCurrentProcess(), g_radar_sites[i], kPatchLen);
      VirtualProtect(g_radar_sites[i], kPatchLen, old_prot, &old_prot);
    }
    g_radar_sites[i] = nullptr;
  }
  g_radar_patched = false;
}

void try_radar_live_once() {
  if (want_pipeline() || !want_radar_live() || g_radar_finished ||
      g_radar_patched) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    if (!g_radar_pending_logged) {
      log_kv("radar_live", "client_pending");
      g_radar_pending_logged = true;
    }
    return;
  }
  if (!client_build_matches()) {
    return;
  }
  if (!patch_radar_live(client)) {
    log_kv("radar_live", "not_found");
  }
  g_radar_finished = true;
}

void sample_gates(HMODULE engine2) {
  void* cs = clientstate_ptr(engine2);
  auto* demo = demo_player(engine2);

  std::uint8_t ishltv = 0;
  if (cs) {
    auto* flag =
        reinterpret_cast<std::uint8_t*>(cs) + offsets::kClientStateIsHltv;
    if (!want_pipeline() && want_clear_is_hltv()) {
      *flag = 0;
      ishltv = 0;
    } else {
      ishltv = *flag;
    }
  }

  const std::uint8_t playing = demo[offsets::kDemoPlayerPlaying];
  const std::uint8_t goto_blocked = demo[offsets::kDemoPlayerGotoBlocked];
  std::int32_t skip = -1;
  std::memcpy(&skip, demo + offsets::kDemoPlayerSkipToTick, sizeof(skip));

  // Probe is edge/periodic only — logging every watcher tick during seek (16ms)
  // caused multi-second hitches from disk I/O.
  static ULONGLONG s_last_probe_ms = 0;
  const ULONGLONG now = GetTickCount64();
  const bool edge = ishltv != g_last_ishtlv || goto_blocked != g_last_goto_blocked ||
                    playing != g_last_playing || skip != g_last_skip;
  if (edge || now - s_last_probe_ms > 2000) {
    s_last_probe_ms = now;
    log_kv("probe",
           ("ishtlv=" + std::to_string(ishltv) +
            " playing=" + std::to_string(playing) +
            " goto_blocked=" + std::to_string(goto_blocked) +
            " skip_tick=" + std::to_string(skip) +
            " filter_entries=" + std::to_string(g_filter_entries.load()) +
            " ishtlv_lie=" +
            std::to_string(g_ishtlv_hooked || g_ishtlv_lie_all_patched ? 1 : 0) +
            " allow=" + std::to_string(g_radar_allow_ready ? 1 : 0) +
            " lie_n=" + std::to_string(g_radar_allow_n) +
            " cs=" + (cs ? "1" : "0"))
               .c_str());
  }

  if (ishltv != g_last_ishtlv) {
    log_kv("edge", ("ishtlv " + std::to_string(g_last_ishtlv) + "->" +
                    std::to_string(ishltv))
                       .c_str());
  }
  if (goto_blocked != g_last_goto_blocked) {
    log_kv("edge", ("goto_blocked " + std::to_string(g_last_goto_blocked) +
                    "->" + std::to_string(goto_blocked))
                       .c_str());
  }
  if (playing != g_last_playing) {
    log_kv("edge", ("playing " + std::to_string(g_last_playing) + "->" +
                    std::to_string(playing))
                       .c_str());
  }
  if (skip != g_last_skip) {
    log_kv("edge", ("skip_tick " + std::to_string(g_last_skip) + "->" +
                    std::to_string(skip))
                       .c_str());
    // Seek just finished — force live icon restyle (numbers flash during skip).
    if (g_last_skip != -1 && skip == -1) {
      restyle_radar_after_seek();
    }
  }

  g_last_ishtlv = ishltv;
  g_last_goto_blocked = goto_blocked;
  g_last_playing = playing;
  g_last_skip = skip;
  g_demo_skipping_cached.store(playing != 0 && skip != -1,
                               std::memory_order_relaxed);
}

DWORD WINAPI watcher_main(LPVOID) {
  HMODULE engine2 = GetModuleHandleA(offsets::kEngine2Name);
  std::uint32_t playing_trace_cycle = 0;
  while (!g_stop_watcher.load()) {
    if (engine2) {
      sample_gates(engine2);
    } else {
      engine2 = GetModuleHandleA(offsets::kEngine2Name);
    }
    // Seek: keep the loop tight for skip-edge detection, skip HUD work that
    // calls into game while entities are torn down (was a big hitch).
    if (g_demo_skipping_cached.load(std::memory_order_relaxed)) {
      ++g_tick;
      Sleep(16);
      continue;
    }
    // engine2 may stay byte-identical across a client-only CS2 update. Never
    // run fixed client RVAs until the independent client fingerprint matches.
    if (!client_build_matches()) {
      ++g_tick;
      Sleep(500);
      continue;
    }
    const bool trace = g_last_playing != 0 && playing_trace_cycle < 8;
    auto trace_phase = [&](const char* phase) {
      if (!trace) {
        return;
      }
      char detail[96]{};
      std::snprintf(detail, sizeof(detail), "cycle=%u phase=%s",
                    playing_trace_cycle, phase);
      log_kv("watcher_phase", detail);
    };
    trace_phase("begin");
    trace_phase("before_allowlist");
    if (!want_pipeline()) {
      try_radar_allowlist_once();
    }
    trace_phase("after_allowlist");
    try_radar_live_once();
    trace_phase("after_radar_live");
    if (!want_pipeline()) {
      try_early_icon_style_once();
    }
    trace_phase("after_early_icon");
    if (!want_pipeline()) {
      try_early_radar_style_once();
    }
    trace_phase("after_early_radar");
    trace_phase("before_identity");
    try_local_identity_remap_once();
    trace_phase("after_identity");
    if (!want_pipeline()) {
      poll_radar_fow_work();
    }
    trace_phase("after_fow");
    if (!want_pipeline()) {
      poll_freeze_radar_restyle();
    }
    trace_phase("after_freeze");
    if (!want_pipeline()) {
      force_teammate_colors_no_letters();
    }
    trace_phase("after_colors");
    log_identity_diag_if_due();
    trace_phase("after_diag");
    if (trace) {
      ++playing_trace_cycle;
    }
    ++g_tick;
    Sleep(500);
  }
  return 0;
}

void reset_log_file() {
  std::ofstream out(temp_log_path(), std::ios::trunc | std::ios::binary);
  out << "session_start=1\n";
}

}  // namespace

bool client_build_matches() {
  const int cached = g_client_build_state.load(std::memory_order_acquire);
  if (cached != 0) {
    return cached > 0;
  }

  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return false;
  }
  g_crash_client_base.store(reinterpret_cast<std::uintptr_t>(client),
                            std::memory_order_relaxed);

  const PeFingerprint fp = fingerprint_loaded_module(client);
  const bool match =
      fingerprint_matches(fp, offsets::kExpectedClientSize,
                          offsets::kExpectedClientTimeDateStamp);
  int expected = 0;
  if (g_client_build_state.compare_exchange_strong(
          expected, match ? 1 : -1, std::memory_order_acq_rel)) {
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "actual_size=0x%08X actual_ts=0x%08X expected_size=0x%08X "
                  "expected_ts=0x%08X",
                  fp.size_of_image, fp.time_date_stamp,
                  offsets::kExpectedClientSize,
                  offsets::kExpectedClientTimeDateStamp);
    log_kv("client_build", detail);
    log_kv("build_check", match ? "client_ok" : "client_mismatch");
    if (!match) {
      log_kv("client_result", "disabled_build_mismatch");
    }
  }
  return match;
}

bool install_hooks() {
  if (g_installed.exchange(true)) {
    return true;
  }

  reset_log_file();

  HMODULE engine2 = GetModuleHandleA(offsets::kEngine2Name);
  if (!engine2) {
    log_kv("build_check", "engine2_missing");
    log_kv("result", "hook_failed");
    g_installed = false;
    return false;
  }

  const PeFingerprint fp = fingerprint_loaded_module(engine2);
  char engine_detail[128]{};
  std::snprintf(engine_detail, sizeof(engine_detail),
                "actual_size=0x%08X actual_ts=0x%08X expected_size=0x%08X "
                "expected_ts=0x%08X",
                fp.size_of_image, fp.time_date_stamp,
                offsets::kExpectedEngine2Size,
                offsets::kExpectedEngine2TimeDateStamp);
  log_kv("engine2_build", engine_detail);
  if (!fingerprint_matches(fp, offsets::kExpectedEngine2Size,
                           offsets::kExpectedEngine2TimeDateStamp)) {
    log_kv("build_check", "build_mismatch");
    log_kv("result", "build_mismatch");
    g_installed = false;
    return false;
  }
  log_kv("build_check", "ok");
  install_crash_probe(engine2);

  std::string mode = "probe_observe";
  if (want_clear_is_hltv()) {
    mode = "clear_UNSAFE";
  }
  if (want_nop_filter()) {
    mode += "+nop_filter";
  }
  if (want_count_filter()) {
    mode += "+count_filter";
  }
  if (want_radar_live()) {
    mode += "+radar_live";
  }
  if (want_ishtlv_lie()) {
    mode += "+ishtlv_lie";
  }
  if (want_pipeline()) {
    mode += "+pipeline";
  } else if (want_ishtlv_lie_all()) {
    mode += "+ishtlv_lie_all_UNSAFE";
  }
  log_kv("mode", mode);

  if (want_pipeline()) {
    if (want_clear_is_hltv() || want_ishtlv_lie() ||
        want_ishtlv_lie_all() || want_radar_live() || want_count_filter() ||
        want_nop_filter()) {
      log_kv("pov_pipeline", "legacy_experiment_flags_ignored");
    }
    // Filtered HUD lie first (camera-safe). Identity remap installs later
    // when demo playing=1 (see identity.cpp) to avoid aborting client init.
    if (!install_ishtlv_lie_filtered(engine2)) {
      log_kv("result", "hook_failed");
      g_installed = false;
      return false;
    }
    // V2 scopes replace the return-address allowlist. The legacy allowlist is
    // still available for LIVE_HUD_ISHLTV_LIE without pipeline mode.
  } else if (want_ishtlv_lie_all()) {
    if (!patch_ishtlv_lie_all(engine2)) {
      log_kv("result", "hook_failed");
      g_installed = false;
      return false;
    }
  } else if (want_ishtlv_lie()) {
    if (!install_ishtlv_lie_filtered(engine2)) {
      log_kv("result", "hook_failed");
      g_installed = false;
      return false;
    }
    try_radar_allowlist_once();
  }

  if (!want_pipeline() && want_count_filter()) {
    if (!install_filter_counter(engine2)) {
      restore_local_identity_remap();
      restore_ishtlv_lie();
      log_kv("result", "hook_failed");
      g_installed = false;
      return false;
    }
  }

  if (!want_pipeline() && want_nop_filter()) {
    // Counting hook steals entry bytes; JE NOP is inside the function and still OK.
    if (!patch_filter_je(engine2)) {
      restore_local_identity_remap();
      restore_filter_counter();
      restore_ishtlv_lie();
      log_kv("result", "hook_failed");
      g_installed = false;
      return false;
    }
  } else {
    log_kv("net_gate", "filter_je_intact");
  }

  // client.dll may load after inject; watcher retries until found or failed.
  try_radar_live_once();
  try_local_identity_remap_once();

  g_stop_watcher = false;
  g_watcher_thread = CreateThread(nullptr, 0, watcher_main, nullptr, 0, nullptr);
  if (!g_watcher_thread) {
    restore_radar_live();
    restore_local_identity_remap();
    restore_filter_je();
    restore_filter_counter();
    restore_ishtlv_lie();
    log_kv("result", "hook_failed");
    g_installed = false;
    return false;
  }

  sample_gates(engine2);
  log_kv("result", "ok");
  return true;
}

bool hud_ranges_ready() { return g_hud_identity_ready; }

bool addr_in_hud_identity_window(std::uintptr_t addr) {
  return g_hud_identity_ready && addr_in_hud_windows(addr);
}

bool addr_in_ishtlv_lie_allowlist(std::uintptr_t addr) {
  if (!g_radar_allow_ready) {
    return false;
  }
  for (int i = 0; i < g_radar_allow_n; ++i) {
    if (addr >= g_radar_allow[i].begin && addr < g_radar_allow[i].end) {
      return true;
    }
  }
  return false;
}

bool demo_is_skipping() {
  return g_demo_skipping_cached.load(std::memory_order_relaxed);
}

void remove_hooks() {
  if (!g_installed.exchange(false)) {
    return;
  }
  g_stop_watcher = true;
  if (g_watcher_thread) {
    WaitForSingleObject(g_watcher_thread, 2000);
    CloseHandle(g_watcher_thread);
    g_watcher_thread = nullptr;
  }
  restore_radar_live();
  restore_local_identity_remap();
  restore_filter_je();
  restore_filter_counter();
  restore_ishtlv_lie();
  remove_crash_probe();
}

}  // namespace live_hud
