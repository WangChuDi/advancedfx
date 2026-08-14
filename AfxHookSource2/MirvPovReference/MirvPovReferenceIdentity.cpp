#include "MirvPovReferenceIdentity.h"

#include "MirvPovReferenceCompat.h"
#include "MirvPovReferenceHooks.h"
#include "MirvPovReferenceNativePipeline.h"
#include "MirvPovReferencePov.h"
#include "MirvPovReferenceOffsets.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace live_hud {
namespace {

#pragma pack(push, 1)
struct RelJump {
  std::uint8_t op{0xE9};
  std::int32_t rel = 0;
};
struct AbsJump {
  std::uint8_t mov_rax[2]{0x48, 0xB8};
  std::uint64_t imm = 0;
  std::uint8_t jmp_rax[2]{0xFF, 0xE0};
};
#pragma pack(pop)

using SlotPawnFn = void*(__fastcall*)(int slot);
using IsObserverFn = bool(__fastcall*)(void* pawn);
using PushNoticeFn = std::int64_t(__fastcall*)(void* hud, char* msg,
                                               unsigned slot,
                                               std::uint8_t* flags);
using SlotEntityFn = void*(__fastcall*)(unsigned slot);
using HudMoneyUpdateFn = void(__fastcall*)(void* hud);
using VoiceStatusUpdateFn = std::uint64_t(__fastcall*)(void* hud);
using VoiceModeFn = int(__fastcall*)(void* game_mode);
using VoiceSpeakingFn = bool(__fastcall*)(void* voice_state, unsigned slot);
using VoiceUpdateSpeakerStatusFn = std::int64_t(__fastcall*)(
    void* voice_status, unsigned ent, int split_screen_slot,
    std::uint8_t talking);
using PlayerPawnEventFn = void(__fastcall*)(void* listener, void* event);
using TeamCounterApplyPlayerDataFn = void(__fastcall*)(void* hud, void* data);
using TeamCounterResolvePawnFn = void*(__fastcall*)(int player_id);

bool g_remap_hooked = false;
bool g_ctrl_hooked = false;
bool g_isobs_hooked = false;
bool g_money_hooked = false;
bool g_money_sticky_hooked = false;
bool g_push_notice_hooked = false;
bool g_hud_money_hooked = false;
bool g_voice_update_hooked = false;
bool g_remap_done = false;
bool g_remap_pending_logged = false;
bool g_early_radar_done = false;
SlotPawnFn g_slot_tramp = nullptr;
SlotPawnFn g_ctrl_tramp = nullptr;
IsObserverFn g_isobs_tramp = nullptr;
IsObserverFn g_money_tramp = nullptr;  // same bool(void*) ABI as 0x85B6C0
using MoneyStickyFn = bool(__fastcall*)();
MoneyStickyFn g_money_sticky_tramp = nullptr;
using GetHudPlayerFn = void*(__fastcall*)();
GetHudPlayerFn g_get_hud_player_tramp = nullptr;
GetHudPlayerFn g_get_hud_alive_tramp = nullptr;
std::uint8_t* g_slot_entry = nullptr;
std::uint8_t* g_ctrl_entry = nullptr;
std::uint8_t* g_isobs_entry = nullptr;
std::uint8_t* g_money_entry = nullptr;
std::uint8_t* g_money_sticky_entry = nullptr;
std::uint8_t* g_get_hud_alive_entry = nullptr;
std::uint8_t* g_slot_tramp_mem = nullptr;
std::uint8_t* g_ctrl_tramp_mem = nullptr;
std::uint8_t* g_isobs_tramp_mem = nullptr;
std::uint8_t* g_money_tramp_mem = nullptr;
std::uint8_t* g_money_sticky_tramp_mem = nullptr;
std::uint8_t* g_get_hud_alive_tramp_mem = nullptr;
std::uint8_t* g_flash_hud_tramp_mem = nullptr;
std::uint8_t* g_flash_scale_stub = nullptr;
std::uint8_t* g_flash_scale_addr = nullptr;
std::uint8_t g_flash_scale_saved[8]{};
bool g_flash_scale_patched = false;
std::uint8_t* g_flash_capture_stub = nullptr;
std::uint8_t* g_flash_capture_addr = nullptr;
std::uint8_t g_flash_capture_saved[8]{};
bool g_flash_capture_patched = false;
struct alignas(8) ViewFlashCapture {
  std::atomic<std::uint32_t> bits{0};
  std::atomic<std::uint32_t> seq{0};
};
ViewFlashCapture g_view_flash{};
std::uint8_t* g_hook_stub = nullptr;
std::uint8_t* g_ctrl_hook_stub = nullptr;
std::uint8_t* g_isobs_hook_stub = nullptr;
std::uint8_t* g_money_hook_stub = nullptr;
std::uint8_t* g_money_sticky_hook_stub = nullptr;
std::uint8_t* g_flash_hud_hook_stub = nullptr;
std::uint8_t* g_get_hud_alive_hook_stub = nullptr;
std::uint8_t g_slot_stolen[16]{};
std::uint8_t g_ctrl_stolen[16]{};
std::uint8_t g_isobs_stolen[16]{};
std::uint8_t g_money_stolen[16]{};
std::uint8_t g_money_sticky_stolen[16]{};
std::uint8_t g_get_hud_alive_stolen[16]{};
bool g_get_hud_alive_hooked = false;
std::uint8_t* g_grenade_pip_addr = nullptr;
std::uint8_t g_grenade_pip_saved[8]{};
bool g_grenade_pip_patched = false;
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_ctrl_calls{0};
std::atomic<std::uint64_t> g_ctrl_hits{0};
std::atomic<std::uint64_t> g_isobs_lies{0};
std::atomic<std::uint64_t> g_money_lies{0};
std::atomic<std::uint64_t> g_money_sticky_allow{0};
std::atomic<std::uint64_t> g_money_sticky_block{0};
std::atomic<std::uint64_t> g_cart_shown{0};
std::atomic<std::uint64_t> g_cart_hidden{0};
std::atomic<std::uint64_t> g_hud_money_updates{0};
std::atomic<bool> g_hud_money_runtime_disabled{false};
std::atomic<std::uint64_t> g_voice_updates{0};
std::atomic<std::uint64_t> g_voice_seek_skips{0};
std::atomic<std::uint64_t> g_voice_team_drops{0};
std::atomic<std::uint64_t> g_voice_filter_faults{0};
std::atomic<bool> g_voice_runtime_disabled{false};
std::atomic<std::uint64_t> g_voice_packet_speakers{0};
std::atomic<std::uint64_t> g_throw_event_calls{0};
std::atomic<std::uint64_t> g_throw_notice_shown{0};
std::atomic<std::uint64_t> g_throw_notice_team_drops{0};
std::atomic<std::uint64_t> g_pawn_hurt_transactions{0};
std::atomic<std::uint64_t> g_pawn_death_transactions{0};
std::atomic<std::uint64_t> g_kill_cash_notices{0};
std::atomic<std::uint64_t> g_teamcounter_player_data_calls{0};
std::atomic<std::uint64_t> g_teamcounter_enemy_filters{0};
std::atomic<std::uint64_t> g_teamcounter_filter_faults{0};
std::atomic<std::uint64_t> g_flash_hud_hits{0};
std::atomic<std::uint64_t> g_hud_alive_calls{0};
std::atomic<std::uint64_t> g_hud_alive_remaps{0};
std::atomic<std::uint64_t> g_hud_alive_gate_ok{0};
std::atomic<std::uint64_t> g_hud_alive_no_follow{0};
int g_alive_rva_logs = 0;
std::uint8_t g_last_freeze = 0xFF;
std::uint8_t g_radar_freeze_seen = 0xFF;
std::atomic<std::uint64_t> g_remap_hits{0};
std::atomic<std::uint64_t> g_no_obs{0};
std::atomic<std::uint64_t> g_no_target{0};
std::atomic<std::uint64_t> g_bad_handle{0};
std::atomic<std::uint64_t> g_null_pawn{0};
std::atomic<std::uint64_t> g_obs_pawn_fallback{0};
std::atomic<std::uint64_t> g_hltv_target_hits{0};
std::uint64_t g_last_diag_calls = 0;
std::uint32_t g_last_fail_handle = 0;
std::uint32_t g_last_obs_handle = 0;
std::int32_t g_last_hltv_idx = 0;
std::uint32_t g_last_logged_obs = 0xFFFFFFFEu;
std::int32_t g_last_logged_hltv = 0x7FFFFFFF;
int g_fail_handle_logs = 0;
std::uint8_t g_last_follow_team = 0xFF;
using RadarDirtyFn = void(__fastcall*)();
using FindHudElementFn = void*(__fastcall*)(const char* name);
RadarDirtyFn g_radar_mark_dirty = nullptr;
FindHudElementFn g_find_hud_element = nullptr;

// Decoded from sub_926D60's `mov r9,[rip+disp]` — chunk-pointer table*.
std::uintptr_t* g_entity_chunk_table_ptr = nullptr;
// Controller pointer table (lea r8,[dwLocalPlayerController]); slot 0 == local.
std::uintptr_t* g_controller_table = nullptr;

// First complete instructions with no RIP-relative operands:
//   sub rsp,28h ; cmp ecx,-1   (7 bytes). Enough for a near E9 hook.
constexpr std::size_t kStolen = 7;

bool env_flag_one(const char* name) {
  char buf[8]{};
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && buf[0] == '1' && buf[1] == '\0';
}

// True when unset or "1"; false only for explicit "0".
bool env_flag_default_on(const char* name) {
  char buf[8]{};
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  if (n == 0) {
    return true;
  }
  return !(buf[0] == '0' && (n == 1 || buf[1] == '\0'));
}

std::uint8_t* find_pattern(std::uint8_t* base, std::uint32_t size,
                           const std::uint8_t* pat, const char* mask) {
  const std::size_t len = std::strlen(mask);
  if (size < len) {
    return nullptr;
  }
  for (std::uint32_t i = 0; i + len <= size; ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < len; ++j) {
      if (mask[j] == 'x' && base[i + j] != pat[j]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return base + i;
    }
  }
  return nullptr;
}

std::uint32_t module_size(HMODULE mod) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return 0;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      reinterpret_cast<const std::uint8_t*>(mod) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return 0;
  }
  return nt->OptionalHeader.SizeOfImage;
}

void* alloc_near(void* near_addr, std::size_t size) {
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  auto page = static_cast<std::uintptr_t>(si.dwAllocationGranularity);
  if (page == 0) {
    page = 0x10000;
  }
  const auto center = reinterpret_cast<std::uintptr_t>(near_addr);
  const auto lo_bound =
      reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
  const auto hi_bound =
      reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
  const std::uintptr_t span = 0x70000000ull;

  auto try_at = [&](std::uintptr_t addr) -> void* {
    if (addr < lo_bound || addr > hi_bound - size) {
      return nullptr;
    }
    addr &= ~(page - 1);
    return VirtualAlloc(reinterpret_cast<void*>(addr), size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  };

  for (std::uintptr_t delta = page; delta < span; delta += page) {
    if (void* p = try_at(center + delta)) {
      return p;
    }
    if (center > delta) {
      if (void* p = try_at(center - delta)) {
        return p;
      }
    }
  }
  return nullptr;
}

bool write_rel_jump(std::uint8_t* from, const void* to) {
  const std::intptr_t rel =
      reinterpret_cast<const std::uint8_t*>(to) - (from + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    return false;
  }
  RelJump j{};
  j.rel = static_cast<std::int32_t>(rel);
  std::memcpy(from, &j, sizeof(j));
  return true;
}

void write_abs_jump(std::uint8_t* from, const void* to) {
  AbsJump j{};
  j.imm = reinterpret_cast<std::uint64_t>(to);
  std::memcpy(from, &j, sizeof(j));
}

void* entity_from_entry(std::uint8_t* entry, std::uint32_t handle,
                        bool require_serial) {
  if (!entry) {
    return nullptr;
  }
  if (require_serial &&
      *reinterpret_cast<std::uint32_t*>(entry + 0x10) != handle) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(entry);
}

// Same walk as sub_926D60: chunk_table = *g_entity_chunk_table_ptr;
// chunk = chunk_table[idx>>9]; entry = chunk + 0x70*(idx&0x1FF).
void* entity_from_handle_game(std::uint32_t handle, bool require_serial) {
  if (!g_entity_chunk_table_ptr) {
    return nullptr;
  }
  const std::uintptr_t table = *g_entity_chunk_table_ptr;
  if (!table) {
    return nullptr;
  }
  const unsigned index = handle & 0x7FFFu;
  const std::uintptr_t chunk =
      *reinterpret_cast<std::uintptr_t*>(table + 0x8ull * (index >> 9));
  if (!chunk) {
    return nullptr;
  }
  auto* entry = reinterpret_cast<std::uint8_t*>(
      chunk + 0x70ull * (index & 0x1FFu));
  return entity_from_entry(entry, handle, require_serial);
}

// MulNX fallback: *(client+dwEntityList) entity system; chunks at +0x10.
void* entity_from_handle_mulnx(HMODULE client, std::uint32_t handle,
                               bool require_serial) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  const std::uintptr_t list =
      *reinterpret_cast<std::uintptr_t*>(base + offsets::kClientDwEntityList);
  if (!list) {
    return nullptr;
  }
  const unsigned index = handle & 0x7FFFu;
  const std::uintptr_t chunk = *reinterpret_cast<std::uintptr_t*>(
      list + 0x8ull * (index >> 9) + 0x10);
  if (!chunk) {
    return nullptr;
  }
  auto* entry = reinterpret_cast<std::uint8_t*>(
      chunk + 0x70ull * (index & 0x1FFu));
  return entity_from_entry(entry, handle, require_serial);
}

void* entity_from_handle(HMODULE client, std::uint32_t handle) {
  if (handle == 0 || handle == 0xFFFFFFFFu || handle == 0xFFFFFFFEu) {
    return nullptr;
  }
  if (void* e = entity_from_handle_game(handle, true)) {
    return e;
  }
  if (void* e = entity_from_handle_mulnx(client, handle, true)) {
    return e;
  }
  if (void* e = entity_from_handle_game(handle, false)) {
    return e;
  }
  return entity_from_handle_mulnx(client, handle, false);
}

void* entity_from_handle(HMODULE client, std::uint32_t handle);

void* pawn_from_controller(HMODULE client, void* controller);

// Entity index (HLTV primary target) — no serial; soft match on entry.
void* entity_from_index(HMODULE client, std::int32_t index) {
  if (index <= 0 || index == -1) {
    return nullptr;
  }
  return entity_from_handle_game(static_cast<std::uint32_t>(index), false);
}

void* as_playable_pawn(HMODULE client, void* ent) {
  if (!ent) {
    return nullptr;
  }
  if (void* p = pawn_from_controller(client, ent)) {
    return p;
  }
  return ent;
}

void* hltv_primary_pawn(HMODULE client) {
  if (!client) {
    return nullptr;
  }
  auto* cam = reinterpret_cast<std::uint8_t*>(client) + offsets::kClientHltvCamera;
  const auto idx = *reinterpret_cast<std::int32_t*>(
      cam + offsets::kHltvCameraPrimaryTarget);
  g_last_hltv_idx = idx;
  void* ent = entity_from_index(client, idx);
  void* pawn = as_playable_pawn(client, ent);
  if (pawn) {
    ++g_hltv_target_hits;
  }
  return pawn;
}

void note_fail_handle(std::uint32_t handle) {
  g_last_fail_handle = handle;
  if (g_fail_handle_logs >= 8) {
    return;
  }
  ++g_fail_handle_logs;
  char msg[48]{};
  std::snprintf(msg, sizeof(msg), "fail_handle=0x%08X", handle);
  log_kv("identity", msg);
}

void* controller_by_slot(int slot) {
  if (!g_controller_table || slot < 0) {
    return nullptr;
  }
  return reinterpret_cast<void*>(g_controller_table[slot]);
}

// HLTV/demo local often has m_hPawn empty; observer body is m_hObserverPawn.
void* pawn_from_controller(HMODULE client, void* controller) {
  if (!controller) {
    return nullptr;
  }
  auto* c = reinterpret_cast<std::uint8_t*>(controller);
  const std::uint32_t handles[3] = {
      *reinterpret_cast<std::uint32_t*>(c + offsets::kControllerPlayerPawn),
      *reinterpret_cast<std::uint32_t*>(c + offsets::kControllerPawnHandle),
      *reinterpret_cast<std::uint32_t*>(c + offsets::kControllerObserverPawn),
  };
  for (std::uint32_t h : handles) {
    if (void* p = entity_from_handle(client, h)) {
      return p;
    }
  }
  return nullptr;
}

void* resolve_local_pawn(HMODULE client, int slot, void* tramp_pawn) {
  if (tramp_pawn) {
    return tramp_pawn;
  }
  int use_slot = slot;
  if (use_slot < 0) {
    use_slot = 0;
  }
  void* ctrl = controller_by_slot(use_slot);
  void* pawn = pawn_from_controller(client, ctrl);
  if (pawn) {
    ++g_obs_pawn_fallback;
    return pawn;
  }
  return hltv_primary_pawn(client);
}

void publish_local_pawn(HMODULE client, void* pawn) {
  if (!client || !pawn) {
    return;
  }
  *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(client) +
                            offsets::kClientDwLocalPlayerPawn) = pawn;
}

void publish_local_controller(HMODULE client, void* controller) {
  if (!client || !controller) {
    return;
  }
  *reinterpret_cast<void**>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientDwLocalPlayerController) = controller;
}

void* controller_from_pawn(HMODULE client, void* pawn) {
  if (!client || !pawn) {
    return nullptr;
  }
  auto* p = reinterpret_cast<std::uint8_t*>(pawn);
  const std::uint32_t handles[2] = {
      *reinterpret_cast<std::uint32_t*>(p + offsets::kPawnOriginalController),
      *reinterpret_cast<std::uint32_t*>(p + offsets::kPawnController),
  };
  for (std::uint32_t h : handles) {
    if (void* c = entity_from_handle(client, h)) {
      return c;
    }
  }
  return nullptr;
}

std::atomic<std::uint64_t> g_kill_gate_remaps{0};
std::atomic<int> g_fow_suppress_left{0};  // diag: 1 when enemy-hide stub live
void* g_fow_follow_pawn = nullptr;

bool patch_bytes(std::uint8_t* addr, const std::uint8_t* expect, std::size_t len,
                 const std::uint8_t* patch, std::uint8_t* saved,
                 const char* tag) {
  if (!addr || !expect || !patch || !saved || len == 0) {
    return false;
  }
  if (std::memcmp(addr, expect, len) != 0) {
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_bytes_mismatch", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  DWORD old_prot = 0;
  if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_protect_failed", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  std::memcpy(saved, addr, len);
  std::memcpy(addr, patch, len);
  FlushInstructionCache(GetCurrentProcess(), addr, len);
  VirtualProtect(addr, len, old_prot, &old_prot);
  char msg[64]{};
  std::snprintf(msg, sizeof(msg), "%s_ok", tag);
  log_kv("pipeline_patch", msg);
  return true;
}

void restore_bytes(std::uint8_t* addr, const std::uint8_t* saved, std::size_t len) {
  if (!addr || !saved || len == 0) {
    return;
  }
  DWORD old_prot = 0;
  if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
    return;
  }
  std::memcpy(addr, saved, len);
  FlushInstructionCache(GetCurrentProcess(), addr, len);
  VirtualProtect(addr, len, old_prot, &old_prot);
}

bool install_rel_call_wrapper(HMODULE module, std::uint32_t call_rva,
                              const std::uint8_t expected[5],
                              const void* wrapper, std::uint8_t** call_out,
                              std::uint8_t saved_out[5],
                              std::uint8_t** stub_out, const char* tag) {
  if (!module || !expected || !wrapper || !call_out || !saved_out ||
      !stub_out) {
    return false;
  }
  auto* call = reinterpret_cast<std::uint8_t*>(module) + call_rva;
  auto* stub = reinterpret_cast<std::uint8_t*>(alloc_near(call, 32));
  if (!stub) {
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_stub_alloc_failed", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  write_abs_jump(stub, wrapper);
  FlushInstructionCache(GetCurrentProcess(), stub, sizeof(AbsJump));
  std::uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
  const std::intptr_t rel = stub - (call + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    VirtualFree(stub, 0, MEM_RELEASE);
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_call_oob", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  *reinterpret_cast<std::int32_t*>(patch + 1) =
      static_cast<std::int32_t>(rel);
  if (!patch_bytes(call, expected, 5, patch, saved_out, tag)) {
    VirtualFree(stub, 0, MEM_RELEASE);
    return false;
  }
  *call_out = call;
  *stub_out = stub;
  return true;
}

bool install_entry_hook(HMODULE module, std::uint32_t rva,
                        const std::uint8_t* expect, std::size_t stolen_len,
                        const void* hook_fn, std::uint8_t** entry_out,
                        std::uint8_t* saved_out, std::uint8_t** tramp_mem_out,
                        std::uint8_t** hook_stub_out, void** tramp_fn_out,
                        const char* tag) {
  if (!module || !expect || !hook_fn || !entry_out || !saved_out ||
      !tramp_mem_out || !hook_stub_out || !tramp_fn_out || stolen_len < 5 ||
      stolen_len > 16) {
    return false;
  }
  auto* entry = reinterpret_cast<std::uint8_t*>(module) + rva;
  if (std::memcmp(entry, expect, stolen_len) != 0) {
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_prologue_mismatch", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  std::memcpy(saved_out, entry, stolen_len);
  auto* mem = reinterpret_cast<std::uint8_t*>(alloc_near(entry, 64));
  if (!mem) {
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_tramp_alloc_failed", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  std::memcpy(mem, saved_out, stolen_len);
  if (!write_rel_jump(mem + stolen_len, entry + stolen_len)) {
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_tramp_back_oob", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  auto* stub = mem + 32;
  write_abs_jump(stub, hook_fn);
  DWORD old_prot = 0;
  if (!VirtualProtect(entry, stolen_len, PAGE_EXECUTE_READWRITE, &old_prot)) {
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_protect_failed", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  if (!write_rel_jump(entry, stub)) {
    VirtualProtect(entry, stolen_len, old_prot, &old_prot);
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[80]{};
    std::snprintf(msg, sizeof(msg), "%s_hook_jmp_oob", tag);
    log_kv("pipeline_patch", msg);
    return false;
  }
  for (std::size_t i = 5; i < stolen_len; ++i) {
    entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), entry, stolen_len);
  FlushInstructionCache(GetCurrentProcess(), mem, 64);
  VirtualProtect(entry, stolen_len, old_prot, &old_prot);
  *entry_out = entry;
  *tramp_mem_out = mem;
  *hook_stub_out = stub;
  *tramp_fn_out = mem;
  char msg[80]{};
  std::snprintf(msg, sizeof(msg), "%s_ok", tag);
  log_kv("pipeline_patch", msg);
  return true;
}

void restore_entry_hook(std::uint8_t*& entry, std::uint8_t* saved,
                        std::size_t stolen_len, std::uint8_t*& tramp_mem,
                        std::uint8_t*& hook_stub) {
  if (entry) {
    restore_bytes(entry, saved, stolen_len);
  }
  if (tramp_mem) {
    VirtualFree(tramp_mem, 0, MEM_RELEASE);
  }
  entry = nullptr;
  tramp_mem = nullptr;
  hook_stub = nullptr;
}

std::uint8_t* g_radar_enemy_hide_addr = nullptr;
std::uint8_t g_radar_enemy_hide_saved[7]{};
bool g_radar_enemy_hide_patched = false;
std::uint8_t* g_radar_enemy_hide_stub = nullptr;

std::uint8_t* g_kill_cmp_addr = nullptr;
std::uint8_t g_kill_cmp_saved[5]{};
bool g_kill_cmp_patched = false;
std::uint8_t* g_kill_match_stub = nullptr;
std::uint8_t* g_kill_match_abs = nullptr;

std::uint8_t* g_radio_mute_jne_addr = nullptr;
std::uint8_t g_radio_mute_jne_saved[6]{};
bool g_radio_mute_jne_patched = false;
std::uint8_t* g_chat_demo_jne_addr = nullptr;
std::uint8_t g_chat_demo_jne_saved[6]{};
bool g_chat_demo_jne_patched = false;
std::uint8_t* g_saytext_demo_jne_addr = nullptr;
std::uint8_t g_saytext_demo_jne_saved[6]{};
bool g_saytext_demo_jne_patched = false;

std::uint8_t* g_push_notice_entry = nullptr;
std::uint8_t g_push_notice_stolen[16]{};
std::uint8_t* g_push_notice_tramp_mem = nullptr;
std::uint8_t* g_push_notice_hook_stub = nullptr;
PushNoticeFn g_push_notice_tramp = nullptr;
std::atomic<std::uint64_t> g_chat_notice_drop{0};
std::atomic<std::uint64_t> g_chat_notice_pass{0};

void** g_player_pawn_event_vtable_slot = nullptr;
PlayerPawnEventFn g_player_pawn_event_original = nullptr;
bool g_player_pawn_event_hooked = false;

std::uint8_t* g_teamcounter_live_call_addr = nullptr;
std::uint8_t g_teamcounter_live_call_saved[5]{};
std::uint8_t* g_teamcounter_live_call_stub = nullptr;
std::uint8_t* g_teamcounter_player_data_call_addr = nullptr;
std::uint8_t g_teamcounter_player_data_call_saved[5]{};
std::uint8_t* g_teamcounter_player_data_call_stub = nullptr;
TeamCounterApplyPlayerDataFn g_teamcounter_apply_player_data_original = nullptr;

std::uint8_t* g_voice_packet_call_addr = nullptr;
std::uint8_t g_voice_packet_call_saved[5]{};
std::uint8_t* g_voice_packet_call_stub = nullptr;
VoiceUpdateSpeakerStatusFn g_voice_update_speaker_original = nullptr;

std::uint8_t* g_hud_money_dispatch_addr = nullptr;
std::uint8_t g_hud_money_dispatch_saved[5]{};
std::uint8_t* g_hud_money_dispatch_stub = nullptr;
HudMoneyUpdateFn g_hud_money_original = nullptr;

std::uint8_t* g_voice_should_draw_addr = nullptr;
std::uint8_t g_voice_should_draw_saved[3]{};
bool g_voice_should_draw_patched = false;
std::uint8_t* g_voice_update_dispatch_addr = nullptr;
std::uint8_t g_voice_update_dispatch_saved[5]{};
std::uint8_t* g_voice_update_dispatch_stub = nullptr;
VoiceStatusUpdateFn g_voice_update_original = nullptr;
std::uint8_t* g_voice_mode_call_addr = nullptr;
std::uint8_t g_voice_mode_call_saved[5]{};
std::uint8_t* g_voice_mode_call_stub = nullptr;
VoiceModeFn g_voice_mode_original = nullptr;
std::uint8_t* g_voice_speaking_call_addr = nullptr;
std::uint8_t g_voice_speaking_call_saved[5]{};
std::uint8_t* g_voice_speaking_call_stub = nullptr;
VoiceSpeakingFn g_voice_speaking_original = nullptr;
bool g_voice_team_filter_patched = false;

std::uint8_t* g_kill_cvar_jne_addr = nullptr;
std::uint8_t g_kill_cvar_jne_saved[6]{};
bool g_kill_cvar_jne_patched = false;

std::uint8_t* g_kill_mode_jne_addr = nullptr;
std::uint8_t g_kill_mode_jne_saved[2]{};
bool g_kill_mode_jne_patched = false;

std::uint8_t* g_kill_mode_je_addr = nullptr;
std::uint8_t g_kill_mode_je_saved[2]{};
bool g_kill_mode_je_patched = false;

std::uint8_t* g_kill_fallback_je_addr = nullptr;
std::uint8_t g_kill_fallback_je_saved[2]{};
bool g_kill_fallback_je_patched = false;

std::uint8_t* g_icon_obs_jne_addr = nullptr;
std::uint8_t g_icon_obs_jne_saved[6]{};
bool g_icon_obs_jne_patched = false;
std::uint8_t* g_icon_hltv_jne_addr = nullptr;
std::uint8_t g_icon_hltv_jne_saved[6]{};
bool g_icon_hltv_jne_patched = false;
std::uint8_t* g_icon_paint_obs_jne_addr = nullptr;
std::uint8_t g_icon_paint_obs_jne_saved[2]{};
bool g_icon_paint_obs_jne_patched = false;
std::uint8_t* g_icon_paint_hltv_jne_addr = nullptr;
std::uint8_t g_icon_paint_hltv_jne_saved[2]{};
bool g_icon_paint_hltv_jne_patched = false;

std::uint8_t* g_flash_spec_opacity_addr = nullptr;
std::uint8_t g_flash_spec_opacity_saved[8]{};
bool g_flash_spec_opacity_patched = false;

std::uint8_t* g_flash_live_composite_addr = nullptr;
std::uint8_t g_flash_live_composite_saved[2]{};
bool g_flash_live_composite_patched = false;

std::atomic<float*> g_spectator_flash_opacity_value{nullptr};
std::atomic<std::uint64_t> g_icon_styles_invalidated{0};

HWND g_flash_wash_hwnd = nullptr;
HWND g_game_hwnd = nullptr;
HBITMAP g_flash_wash_bmp = nullptr;
void* g_flash_wash_bits = nullptr;
int g_flash_wash_w = 0;
int g_flash_wash_h = 0;
float g_flash_wash_last_a = -1.f;

std::atomic<std::uint64_t> g_kill_match_checks{0};
std::atomic<std::uint64_t> g_kill_match_hits{0};
std::atomic<bool> g_pending_kill_card{false};
std::atomic<bool> g_pending_kill_headshot{false};
std::atomic<std::uint64_t> g_kill_card_plays{0};
std::atomic<std::uint64_t> g_colors_forced{0};
std::atomic<int*> g_teammate_colors_value{nullptr};
std::atomic<int*> g_radar_show_all_value{nullptr};
std::atomic<std::uint64_t> g_show_all_forced{0};

void* follow_pawn_now(HMODULE client) {
  void* follow = nullptr;
  if (client && g_last_obs_handle != 0 && g_last_obs_handle != 0xFFFFFFFFu &&
      g_last_obs_handle != 0xFFFFFFFEu) {
    follow = entity_from_handle(client, g_last_obs_handle);
  }
  if (!follow) {
    follow = g_fow_follow_pawn;
  }
  return follow;
}

bool same_player(HMODULE client, void* a, void* b) {
  if (!a || !b) {
    return false;
  }
  if (a == b) {
    return true;
  }
  void* ca = controller_from_pawn(client, a);
  void* cb = controller_from_pawn(client, b);
  if (ca && cb && ca == cb) {
    return true;
  }
  auto* ap = reinterpret_cast<std::uint8_t*>(a);
  auto* bp = reinterpret_cast<std::uint8_t*>(b);
  const std::uint32_t ah =
      *reinterpret_cast<std::uint32_t*>(ap + offsets::kPawnController);
  const std::uint32_t bh =
      *reinterpret_cast<std::uint32_t*>(bp + offsets::kPawnController);
  if (ah != 0 && ah != 0xFFFFFFFFu && ah == bh) {
    return true;
  }
  const std::uint32_t ah2 =
      *reinterpret_cast<std::uint32_t*>(ap + offsets::kPawnOriginalController);
  const std::uint32_t bh2 =
      *reinterpret_cast<std::uint32_t*>(bp + offsets::kPawnOriginalController);
  return ah2 != 0 && ah2 != 0xFFFFFFFFu && ah2 == bh2;
}

// Resolve player entity from an IGameEvent field (same pattern as death handlers).
void* event_player_field(HMODULE client, void* event, const char* field,
                         unsigned seed, int name_len) {
  if (!client || !event || !field) {
    return nullptr;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using FilterFn = void*(__fastcall*)(void*);
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto hash = reinterpret_cast<HashFn>(base + offsets::kClientEventFieldHashRva);
  auto filter =
      reinterpret_cast<FilterFn>(base + offsets::kClientFilterPlayerEntRva);
  void** vt = *reinterpret_cast<void***>(event);
  if (!vt) {
    return nullptr;
  }
  // vtable+0x88: get entity* from field keybuf
  using GetEntFn = void*(__fastcall*)(void* event, void* keybuf);
  auto get_ent = reinterpret_cast<GetEntFn>(vt[0x88 / 8]);
  if (!get_ent) {
    return nullptr;
  }
  // Attacker: hash "cker"/4 with EDE4F213; keybuf name stays "attacker".
  const char* hash_s = field;
  int hash_n = name_len;
  if (name_len == 8 && field[0] == 'a' && std::strcmp(field, "attacker") == 0) {
    hash_s = field + 4;
    hash_n = 4;
  } else if (name_len == 6 && field[0] == 'u' &&
             std::strcmp(field, "userid") == 0) {
    hash_s = field + 4;
    hash_n = 2;
  }
  alignas(16) std::uint8_t keybuf[24]{};
  const int h = hash(hash_s, hash_n, seed);
  *reinterpret_cast<int*>(keybuf + 0) = h;
  *reinterpret_cast<int*>(keybuf + 4) = -1;
  *reinterpret_cast<const char**>(keybuf + 8) = field;
  void* ent = get_ent(event, keybuf);
  if (!ent) {
    return nullptr;
  }
  return filter(ent);
}

const char* event_string_field(HMODULE client, void* event, const char* field,
                               unsigned seed, int name_len) {
  if (!client || !event || !field) {
    return nullptr;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto hash = reinterpret_cast<HashFn>(base + offsets::kClientEventFieldHashRva);
  void** vt = *reinterpret_cast<void***>(event);
  if (!vt) {
    return nullptr;
  }
  using GetStringFn = const char*(__fastcall*)(void* event, void* keybuf,
                                               const char* fallback);
  auto get_string = reinterpret_cast<GetStringFn>(vt[0x50 / 8]);
  if (!get_string) {
    return nullptr;
  }
  const char* hash_s = field;
  int hash_n = name_len;
  if (name_len == 6 && field[0] == 'w' &&
      std::strcmp(field, "weapon") == 0) {
    hash_s = field + 4;
    hash_n = 2;
  }
  alignas(16) std::uint8_t keybuf[24]{};
  *reinterpret_cast<int*>(keybuf + 0) = hash(hash_s, hash_n, seed);
  *reinterpret_cast<int*>(keybuf + 4) = -1;
  *reinterpret_cast<const char**>(keybuf + 8) = field;
  return get_string(event, keybuf, "");
}

bool event_player_slot_matches_pawn(HMODULE client, void* event, void* pawn,
                                    unsigned* slot_out) {
  if (!client || !event || !pawn) {
    return false;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  using PawnSlotFn = int*(__fastcall*)(void* pawn, int* out);
  auto pawn_slot = reinterpret_cast<PawnSlotFn>(
      base + offsets::kClientPawnGetPlayerSlotRva);
  int own = -1;
  pawn_slot(pawn, &own);
  if (own < 0 || own >= 64) {
    return false;
  }
  void** vt = *reinterpret_cast<void***>(event);
  if (!vt) {
    return false;
  }
  // Exact weapon_fire listener path @ C0BF07: vtable+0x78 writes the userid's
  // player slot, then compares it with sub_900910(pawn).
  using GetSlotFn = void(__fastcall*)(void* event, int* out, void* keybuf);
  auto get_slot = reinterpret_cast<GetSlotFn>(vt[0x78 / 8]);
  if (!get_slot) {
    return false;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  auto hash = reinterpret_cast<HashFn>(base + offsets::kClientEventFieldHashRva);
  static const char kUserid[] = "userid";
  alignas(16) std::uint8_t keybuf[24]{};
  *reinterpret_cast<int*>(keybuf + 0) =
      hash(kUserid + 4, 2, offsets::kClientEventUseridSeed);
  *reinterpret_cast<int*>(keybuf + 4) = -1;
  *reinterpret_cast<const char**>(keybuf + 8) = kUserid;
  int event_slot = -1;
  get_slot(event, &event_slot, keybuf);
  if (event_slot != own) {
    return false;
  }
  if (slot_out) {
    *slot_out = static_cast<unsigned>(event_slot);
  }
  return true;
}

// player_death "headshot" bool/int.
// Confirmed @ E017DA: hash("headshot",8,0x3141592E) via vt+0x38 GetInt; setg if >0.
// Also try +0x78/+0x70 used by other handlers.
bool event_field_truthy(HMODULE client, void* event, const char* field,
                        unsigned seed, int name_len) {
  if (!client || !event || !field) {
    return false;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto hash = reinterpret_cast<HashFn>(base + offsets::kClientEventFieldHashRva);
  void** vt = *reinterpret_cast<void***>(event);
  if (!vt) {
    return false;
  }
  alignas(16) std::uint8_t keybuf[24]{};
  const int h = hash(field, name_len, seed);
  *reinterpret_cast<int*>(keybuf + 0) = h;
  *reinterpret_cast<int*>(keybuf + 4) = -1;
  *reinterpret_cast<const char**>(keybuf + 8) = field;
  using GetIntFn = std::int64_t(__fastcall*)(void* event, void* keybuf);
  // Death-notice headshot path uses vt+0x38.
  for (const int off : {0x38, 0x78}) {
    if (auto get_i = reinterpret_cast<GetIntFn>(vt[off / 8])) {
      const auto v = get_i(event, keybuf);
      if (v) {
        return true;
      }
    }
  }
  using GetBoolFn = bool(__fastcall*)(void* event, void* keybuf);
  if (auto get_b = reinterpret_cast<GetBoolFn>(vt[0x70 / 8])) {
    if (get_b(event, keybuf)) {
      return true;
    }
  }
  return false;
}

void play_kill_card(HMODULE client, void* pawn, bool headshot) {
  if (!client || !pawn) {
    return;
  }
  using PlayFn = void(__fastcall*)(void* pawn, const char* name);
  auto play = reinterpret_cast<PlayFn>(
      reinterpret_cast<std::uint8_t*>(client) + offsets::kClientPlayUiSoundRva);
  // Live (client.dll 89DBA0 → 887DB0): lethal hit plays Death* AttackerFeedback
  // first (HS → DeathHeadShot, else DeathBody). Server also delivers
  // UI.KillCard.1 to the attacker — demo never gets that, so synthesize both.
  // Order matches client: Death feedback, then KillCard confirm. HS stays
  // prominent via the DeathHeadShot event, not by dropping KillCard.
  if (headshot) {
    play(pawn, "Player.DeathHeadShot.AttackerFeedback");
  } else {
    play(pawn, "Player.DeathBody.AttackerFeedback");
  }
  play(pawn, "UI.KillCard.1");
}

void pump_deferred_kill_card(HMODULE client) {
  if (!g_pending_kill_card.load(std::memory_order_relaxed)) {
    return;
  }
  if (!g_pending_kill_card.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  const bool headshot =
      g_pending_kill_headshot.exchange(false, std::memory_order_acq_rel);
  if (!client || demo_is_skipping()) {
    return;
  }
  void* follow = follow_pawn_now(client);
  if (!follow) {
    return;
  }
  __try {
    play_kill_card(client, follow, headshot);
    ++g_kill_card_plays;
    log_kv("kill_sound", headshot ? "kill_card_deferred_hs" : "kill_card_deferred");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("kill_sound", "kill_card_deferred_seh");
  }
}

void force_radar_show_all_client(HMODULE client) {
  int* cached = g_radar_show_all_value.load(std::memory_order_acquire);
  if (!cached && client) {
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    __try {
      auto* obj = *reinterpret_cast<std::uint8_t**>(
          base + offsets::kClientRadarShowAllCvarRva + 8);
      if (obj) {
        cached = reinterpret_cast<int*>(obj + 0x58);
        g_radar_show_all_value.store(cached, std::memory_order_release);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      cached = nullptr;
    }
    if (!cached) {
      static bool once = false;
      if (!once) {
        once = true;
        log_kv("radar", "show_all_resolve_failed");
      }
      return;
    }
  }
  if (!cached) {
    return;
  }
  // Steam UpdatePlayerIcon → 898740/898630: if this cvar != 0, every non-self
  // slot returns "enemy" and takes the FoW/spotted path (teammates vanish under
  // IsHLTV-lie without MulNX PlayerSpot). Value 0 uses team compare so same-
  // team icons take the show path. Force 0 (not 1).
  __try {
    const int before = *cached;
    *cached = 0;
    if (before != 0) {
      ++g_show_all_forced;
      if (g_show_all_forced.load() <= 5) {
        char msg[48]{};
        std::snprintf(msg, sizeof(msg), "show_all_val=%d->0", before);
        log_kv("radar", msg);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("radar", "show_all_write_seh");
  }
}

void force_teammate_colors_no_letters_client(HMODULE client) {
  force_radar_show_all_client(client);
  int* cached = g_teammate_colors_value.load(std::memory_order_acquire);
  if (!cached && client) {
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    // Direct layout (no game call): ConVarDesc+8 → obj, value int at obj+0x58
    // — same as GetValue helper @ 0x1862B30. Safe from watcher thread.
    __try {
      auto* obj = *reinterpret_cast<std::uint8_t**>(
          base + offsets::kClientTeammateColorsCvarRva + 8);
      if (obj) {
        cached = reinterpret_cast<int*>(obj + 0x58);
        g_teammate_colors_value.store(cached, std::memory_order_release);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      cached = nullptr;
    }
    if (!cached) {
      static bool once = false;
      if (!once) {
        once = true;
        log_kv("radar", "teammate_colors_resolve_failed");
      }
      return;
    }
  }
  if (!cached) {
    return;
  }
  __try {
    const int before = *cached;
    *cached = 1;  // always force — user config / UI may flip it back to 2
    if (before != 1) {
      ++g_colors_forced;
    }
    if (g_colors_forced.load() <= 5) {
      char msg[64]{};
      std::snprintf(msg, sizeof(msg), "teammate_colors_val=%d->1", before);
      log_kv("radar", msg);
      if (before == 1) {
        ++g_colors_forced;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("radar", "teammate_colors_write_seh");
  }
}

int entity_team(void* ent);
void show_kill_cash_award(HMODULE client, void* victim, void* event);

// Observe-only: queue KillCard when attacker==follow. Must NOT change
// player_death control flow / deathcam (prior rewrite+NOP path crashed).
void __fastcall observe_player_death_kill_card(void* victim,
                                               void* /*victim_ctrl*/,
                                               void* event) {
  ++g_kill_match_checks;
  __try {
    if (demo_is_skipping() || !event) {
      return;
    }
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    void* follow = follow_pawn_now(client);
    if (!follow) {
      if (g_kill_match_checks.load() <= 8) {
        log_kv("kill_sound", "kill_miss no_follow");
      }
      return;
    }
    void* attacker = event_player_field(client, event, "attacker",
                                        offsets::kClientEventAttackerSeed, 8);
    if (!attacker) {
      if (g_kill_match_checks.load() <= 12) {
        log_kv("kill_sound", "kill_miss no_attacker");
      }
      return;
    }
    if (!same_player(client, attacker, follow)) {
      if (g_kill_match_checks.load() <= 12) {
        log_kv("kill_sound", "kill_miss mismatch");
      }
      return;
    }
    // Queue body KillCard first — HS field reads must not abort the match.
    bool hs = false;
    __try {
      // Confirmed death-notice seed (not userid's 0x31415920).
      hs = event_field_truthy(client, event, "headshot",
                             offsets::kClientEventHeadshotSeed, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      hs = false;
    }
    g_pending_kill_headshot.store(hs, std::memory_order_release);
    g_pending_kill_card.store(true, std::memory_order_release);
    if (entity_team(victim) != entity_team(follow)) {
      show_kill_cash_award(client, victim, event);
    }
    ++g_kill_match_hits;
    if (g_kill_match_checks.load() <= 16) {
      log_kv("kill_sound", hs ? "kill_card_queued_hs" : "kill_card_queued");
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("kill_sound", "kill_observe_seh");
  }
}

int entity_team(void* ent) {
  if (!ent) {
    return 0;
  }
  __try {
    return *reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uint8_t*>(ent) + offsets::kEntityTeamNum);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

bool notice_same_team_as_follow(HMODULE client, unsigned slot) {
  if (!client || slot == 0xFFFFFFFFu || slot > 0x7FFFu) {
    return true;  // system / unknown — keep
  }
  void* follow = follow_pawn_now(client);
  if (!follow) {
    return true;
  }
  const int follow_team = entity_team(follow);
  if (follow_team != 2 && follow_team != 3) {
    return true;
  }
  using SlotFn = SlotEntityFn;
  auto slot_ent = reinterpret_cast<SlotFn>(
      reinterpret_cast<std::uint8_t*>(client) + offsets::kClientSlotEntityRva);
  void* speaker = nullptr;
  __try {
    speaker = slot_ent(slot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    speaker = nullptr;
  }
  if (!speaker) {
    return true;
  }
  const int speaker_team = entity_team(speaker);
  if (speaker_team != 2 && speaker_team != 3) {
    return true;
  }
  return speaker_team == follow_team;
}

std::int64_t __fastcall push_notice_hook(void* hud, char* msg, unsigned slot,
                                         std::uint8_t* flags) {
  // Pipeline V2 installs this hook only to retain the first-version trampoline
  // used by derived grenade notices. Native messages must pass through byte-
  // for-byte; team filtering belongs to the native message transaction.
  if (want_pipeline()) {
    return g_push_notice_tramp ? g_push_notice_tramp(hud, msg, slot, flags)
                               : 0;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (client && !demo_is_skipping() &&
      !notice_same_team_as_follow(client, slot)) {
    ++g_chat_notice_drop;
    if (g_chat_notice_drop.load() <= 8) {
      log_kv("chat_notice", "drop_enemy_team");
    }
    return 0;
  }
  ++g_chat_notice_pass;
  // First few calls: dump VoiceStatus notice-panel state. PushNotice silently
  // ignores messages when the panel pool is empty ("no notice panels
  // available"), which would explain chat+voice both missing in demo.
  if (g_chat_notice_pass.load() <= 6 && hud) {
    int free_stack = -1;
    int panel_count = -1;
    __try {
      free_stack = *reinterpret_cast<const int*>(
          reinterpret_cast<const std::uint8_t*>(hud) + 328);
      panel_count = *reinterpret_cast<const int*>(
          reinterpret_cast<const std::uint8_t*>(hud) + 304);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "pass slot=%u free=%d panels=%d", slot,
                  free_stack, panel_count);
    log_kv("chat_notice", buf);
  }
  if (!g_push_notice_tramp) {
    return 0;
  }
  return g_push_notice_tramp(hud, msg, slot, flags);
}

const char* grenade_radio_token(const char* weapon) {
  if (!weapon) {
    return nullptr;
  }
  if (std::strncmp(weapon, "weapon_", 7) == 0) {
    weapon += 7;
  }
  if (std::strcmp(weapon, "flashbang") == 0) {
    return "#SFUI_TitlesTXT_Flashbang";
  }
  if (std::strcmp(weapon, "smokegrenade") == 0) {
    return "#SFUI_TitlesTXT_Smoke_in_the_hole";
  }
  if (std::strcmp(weapon, "hegrenade") == 0) {
    return "#SFUI_TitlesTXT_Fire_in_the_hole";
  }
  if (std::strcmp(weapon, "incgrenade") == 0) {
    return "#SFUI_TitlesTXT_Incendiary_in_the_hole";
  }
  if (std::strcmp(weapon, "molotov") == 0) {
    return "#SFUI_TitlesTXT_Incendiary_in_the_hole";
  }
  if (std::strcmp(weapon, "decoy") == 0) {
    return "#SFUI_TitlesTXT_Decoy_in_the_hole";
  }
  return nullptr;
}

const char* localized_token(HMODULE client, const char* token) {
  if (!client || !token || !token[0]) {
    return nullptr;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  void* localize = *reinterpret_cast<void**>(
      base + offsets::kClientLocalizationInterfaceRva);
  if (!localize) {
    return nullptr;
  }
  void** vt = *reinterpret_cast<void***>(localize);
  if (!vt) {
    return nullptr;
  }
  using LocalizeFn = const char*(__fastcall*)(void*, const char*);
  auto find = reinterpret_cast<LocalizeFn>(vt[120 / 8]);
  if (!find) {
    return nullptr;
  }
  const char* localized = find(localize, token);
  return localized && localized[0] ? localized : nullptr;
}

const char* player_name_from_pawn(HMODULE client, void* pawn) {
  void* controller = controller_from_pawn(client, pawn);
  if (!controller) {
    return nullptr;
  }
  auto* name = reinterpret_cast<const char*>(
      reinterpret_cast<const std::uint8_t*>(controller) +
      offsets::kControllerPlayerName);
  // m_iszPlayerName is inline CNetworkStringBase<128>. Require a terminator so
  // a stale schema offset can never make snprintf walk into adjacent fields.
  for (std::size_t i = 0; i < 128; ++i) {
    if (name[i] == '\0') {
      return i == 0 ? nullptr : name;
    }
  }
  return nullptr;
}

const char* localized_place_name(HMODULE client, void* pawn) {
  if (!client || !pawn) {
    return nullptr;
  }
  const auto* token = reinterpret_cast<const char*>(
      reinterpret_cast<const std::uint8_t*>(pawn) +
      offsets::kPawnLastPlaceName);
  std::size_t len = 0;
  while (len < 18 && token[len]) {
    ++len;
  }
  if (len == 0 || len == 18) {
    return nullptr;
  }
  char key[32] = "#";
  std::memcpy(key + 1, token, len);
  key[len + 1] = '\0';
  const char* localized = localized_token(client, key);
  return localized && localized[0] ? localized : token;
}

bool push_raw_gameplay_notice(HMODULE client, const char* message,
                              unsigned slot = 0xFFFFFFFFu) {
  if (!client || !message || !message[0] || !g_push_notice_tramp) {
    return false;
  }
  using FindFn = void*(__fastcall*)(const char*);
  auto find_hud = reinterpret_cast<FindFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientFindHudElementRva);
  void* voice_element = find_hud("CCSGO_HudVoiceStatus");
  if (!voice_element) {
    return false;
  }
  void* voice_hud = reinterpret_cast<std::uint8_t*>(voice_element) - 0x20;
  char copy[320]{};
  std::snprintf(copy, sizeof(copy), "%s", message);
  // RadioText initializes this option word to 0 and sets only byte +1 before
  // calling ChatPrintf. Preserve that native state at the PushNotice boundary.
  std::uint8_t flags[4] = {0, 1, 0, 0};
  g_push_notice_tramp(voice_hud, copy, slot, flags);
  return true;
}

int kill_cash_reward(const char* weapon) {
  if (!weapon || !weapon[0]) {
    return 300;
  }
  if (std::strncmp(weapon, "weapon_", 7) == 0) {
    weapon += 7;
  }
  if (std::strstr(weapon, "knife") || std::strcmp(weapon, "bayonet") == 0) {
    return 1500;
  }
  if (std::strcmp(weapon, "nova") == 0 ||
      std::strcmp(weapon, "xm1014") == 0 ||
      std::strcmp(weapon, "mag7") == 0 ||
      std::strcmp(weapon, "sawedoff") == 0) {
    return 900;
  }
  if (std::strcmp(weapon, "mp9") == 0 ||
      std::strcmp(weapon, "mac10") == 0 ||
      std::strcmp(weapon, "mp7") == 0 ||
      std::strcmp(weapon, "mp5sd") == 0 ||
      std::strcmp(weapon, "ump45") == 0 ||
      std::strcmp(weapon, "bizon") == 0) {
    return 600;
  }
  if (std::strcmp(weapon, "awp") == 0 ||
      std::strcmp(weapon, "cz75a") == 0 ||
      std::strcmp(weapon, "taser") == 0) {
    return 100;
  }
  return 300;
}

void show_kill_cash_award(HMODULE client, void* /*victim*/, void* event) {
  if (!client || !event || demo_is_skipping()) {
    return;
  }
  const char* weapon = event_string_field(client, event, "weapon",
                                          offsets::kClientEventWeaponSeed, 6);
  const int reward = kill_cash_reward(weapon);
  char message[160]{};
  std::snprintf(
      message, sizeof(message),
      "\x01\xE8\xA7\xA3\xE5\x86\xB3\xE4\xB8\x80\xE5\x90\x8D\xE6\x95\x8C\xE4\xBA\xBA\xE8\x8E\xB7\xE5\xBE\x97 \x04+$%d",
      reward);
  if (push_raw_gameplay_notice(client, message)) {
    const auto shown = ++g_kill_cash_notices;
    if (shown <= 12) {
      char diag[96]{};
      std::snprintf(diag, sizeof(diag), "shown=%llu reward=%d weapon=%s",
                    static_cast<unsigned long long>(shown), reward,
                    weapon ? weapon : "");
      log_kv("kill_cash", diag);
    }
  }
}

void synthesize_grenade_notice(void* listener, void* event) {
  ++g_throw_event_calls;
  if (!listener || !event || demo_is_skipping() || !g_push_notice_tramp) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return;
  }
  auto* listener_pawn = reinterpret_cast<std::uint8_t*>(listener) -
                        offsets::kPlayerPawnEventListenerOffset;
  unsigned event_slot = 0xFFFFFFFFu;
  if (!event_player_slot_matches_pawn(client, event, listener_pawn,
                                      &event_slot)) {
    return;
  }
  const char* weapon = event_string_field(client, event, "weapon",
                                          offsets::kClientEventWeaponSeed, 6);
  const char* notice_token = grenade_radio_token(weapon);
  if (!notice_token) {
    return;
  }
  void* follow = follow_pawn_now(client);
  const int follow_team = entity_team(follow);
  const int event_team = entity_team(listener_pawn);
  if (!follow || (follow_team != 2 && follow_team != 3) ||
      event_team != follow_team) {
    ++g_throw_notice_team_drops;
    return;
  }
  const char* name = player_name_from_pawn(client, listener_pawn);
  if (!name) {
    name = "\xE9\x98\x9F\xE5\x8F\x8B";  // teammate
  }
  const char* place = localized_place_name(client, listener_pawn);
  const char* team_prefix = localized_token(
      client, event_team == 3 ? "#game_radio_team_prefix_3"
                              : "#game_radio_team_prefix_2");
  if (!team_prefix) {
    team_prefix = event_team == 3 ? "[CT] " : "[T] ";
  }
  const char* notice = localized_token(client, notice_token);
  if (!notice) {
    return;
  }
  const char* weapon_name = weapon;
  if (weapon_name && std::strncmp(weapon_name, "weapon_", 7) == 0) {
    weapon_name += 7;
  }
  // Exact live zh-CN radio phrases and color controls. The generic SFUI
  // tokens use legacy ASCII punctuation and call a Molotov "燃烧瓶".
  if (weapon_name && (std::strcmp(weapon_name, "incgrenade") == 0 ||
                      std::strcmp(weapon_name, "molotov") == 0)) {
    notice = "\x10\xE7\x87\x83\xE7\x83\xA7\xE5\xBC\xB9\xEF\xBC\x81";
  } else if (weapon_name && std::strcmp(weapon_name, "hegrenade") == 0) {
    notice = "\x0F\xE9\xAB\x98\xE7\x88\x86\xE6\x89\x8B\xE9\x9B\xB7\xEF\xBC\x81";
  } else if (weapon_name && std::strcmp(weapon_name, "flashbang") == 0) {
    notice = "\x0B\xE5\xB0\x8F\xE5\xBF\x83\xE9\x97\xAA\xE5\x85\x89\xE5\xBC\xB9\xEF\xBC\x81";
  } else if (weapon_name && std::strcmp(weapon_name, "smokegrenade") == 0) {
    notice = "\x05\xE7\x83\x9F\xE9\x9B\xBE\xE5\xBC\xB9\xEF\xBC\x81";
  }
  char flash_notice[128]{};
  if (false && weapon && (std::strcmp(weapon, "flashbang") == 0 ||
                 std::strcmp(weapon, "weapon_flashbang") == 0)) {
    // Unlike the other five SFUI grenade tokens, Flashbang has no embedded
    // radio color or punctuation. The native Chinese radio line renders the
    // warning in blue as “小心!闪光震撼弹!”. Keep other locales language-safe.
    static constexpr char kChineseFlash[] =
        "\xE9\x97\xAA\xE5\x85\x89\xE9\x9C\x87\xE6\x92\xBC\xE5\xBC\xB9";
    if (std::strcmp(notice, kChineseFlash) == 0) {
      std::snprintf(flash_notice, sizeof(flash_notice),
                    "\x0B\xE5\xB0\x8F\xE5\xBF\x83!%s!", notice);
    } else {
      std::snprintf(flash_notice, sizeof(flash_notice), "\x0B%s!", notice);
    }
    notice = flash_notice;
  }
  char message[320]{};
  if (place && place[0]) {
    std::snprintf(message, sizeof(message),
                  " %s\x03%s\x04\xEF\xB9\xAB%s\x01: %s",
                  team_prefix, name, place, notice);
  } else {
    std::snprintf(message, sizeof(message),
                  " %s\x03%s\x01: %s", team_prefix, name,
                  notice);
  }
  if (!push_raw_gameplay_notice(client, message, event_slot)) {
    return;
  }
  const auto shown = ++g_throw_notice_shown;
  if (shown <= 12) {
    char diag[256]{};
    std::snprintf(diag, sizeof(diag),
                  "shown=%llu slot=%u team=%d player=%s weapon=%s",
                  static_cast<unsigned long long>(shown), event_slot,
                  event_team, name, weapon);
    log_kv("throw_notice", diag);
  }
}

enum class PawnCombatEvent {
  none,
  hurt,
  death,
};

PawnCombatEvent followed_pawn_combat_event(void* listener, void* event) {
  if (!listener || !event || demo_is_skipping()) {
    return PawnCombatEvent::none;
  }
  __try {
    auto** vtable = *reinterpret_cast<void***>(event);
    if (!vtable || !vtable[1]) {
      return PawnCombatEvent::none;
    }
    using EventNameFn = const char*(__fastcall*)(void*);
    const char* name = reinterpret_cast<EventNameFn>(vtable[1])(event);
    PawnCombatEvent combat_event = PawnCombatEvent::none;
    if (name && std::strcmp(name, "player_hurt") == 0) {
      combat_event = PawnCombatEvent::hurt;
    } else if (name && std::strcmp(name, "player_death") == 0) {
      combat_event = PawnCombatEvent::death;
    } else {
      return PawnCombatEvent::none;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn) {
      return PawnCombatEvent::none;
    }
    auto* pawn = reinterpret_cast<std::uint8_t*>(listener) -
                 offsets::kPlayerPawnEventListenerOffset;
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    if (!client || !same_player(client, pawn, followed.pawn) ||
        !event_player_slot_matches_pawn(client, event, pawn, nullptr)) {
      return PawnCombatEvent::none;
    }
    return combat_event;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "pawn_event_match_seh");
  }
  return PawnCombatEvent::none;
}

void log_pawn_combat_visual_state(void* listener,
                                  PawnCombatEvent combat_event,
                                  std::uint64_t count) noexcept {
  __try {
    auto* pawn = reinterpret_cast<std::uint8_t*>(listener) -
                 offsets::kPlayerPawnEventListenerOffset;
    char detail[224]{};
    std::snprintf(
        detail, sizeof(detail),
        "%s count=%llu player_state=%u last_damage=%.3f "
        "death_event_state=0x%08X headshot_event_time=%.3f",
        combat_event == PawnCombatEvent::hurt
            ? "pawn_player_hurt_transaction"
            : "pawn_player_death_transaction",
        static_cast<unsigned long long>(count),
        *reinterpret_cast<unsigned*>(pawn + offsets::kPawnPlayerState),
        static_cast<double>(*reinterpret_cast<float*>(
            pawn + offsets::kPawnLastDamageTime)),
        *reinterpret_cast<unsigned*>(pawn + offsets::kPawnDeathEventState),
        static_cast<double>(*reinterpret_cast<float*>(
            pawn + offsets::kPawnHeadshotEventTime)));
    log_kv("pov_combat", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "pawn_visual_probe_seh");
  }
}

void dispatch_native_player_pawn_event(void* listener, void* event,
                                       PawnCombatEvent combat_event) {
  if (g_player_pawn_event_original) {
    if (combat_event != PawnCombatEvent::none) {
      // C_CSPlayerPawn's own listener is the native owner of the local hurt
      // and death visual state. Its player_death branch compares the event
      // victim against GetLocalPlayerPawn; run only the followed victim's
      // callback with live identity/mode semantics and leave every other Pawn
      // listener on honest Demo state.
      pov::Scope scope(pov::Domain::combat_feedback);
      g_player_pawn_event_original(listener, event);
      auto& counter = combat_event == PawnCombatEvent::hurt
                          ? g_pawn_hurt_transactions
                          : g_pawn_death_transactions;
      const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
      if (count <= 24) {
        log_pawn_combat_visual_state(listener, combat_event, count);
      }
    } else {
      g_player_pawn_event_original(listener, event);
    }
  }
}

void __fastcall player_pawn_event_hook(void* listener, void* event) {
  const auto combat_event = followed_pawn_combat_event(listener, event);
  dispatch_native_player_pawn_event(listener, event, combat_event);
  __try {
    synthesize_grenade_notice(listener, event);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (g_throw_event_calls.load() <= 8) {
      char msg[64]{};
      std::snprintf(msg, sizeof(msg), "seh code=0x%08lX",
                    static_cast<unsigned long>(GetExceptionCode()));
      log_kv("throw_notice", msg);
    }
  }
}

bool install_player_pawn_event_hook(HMODULE client) {
  if (g_player_pawn_event_hooked) {
    return true;
  }
  if (!client) {
    return false;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto** slot = reinterpret_cast<void**>(
      base + offsets::kClientPlayerPawnEventVtableSlotRva);
  void* expected = base + offsets::kClientPlayerPawnFireGameEventRva;
  if (*slot != expected) {
    log_kv("pov_boundary", "player_pawn_event_vtable_mismatch");
    return false;
  }
  DWORD old_prot = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
    log_kv("pov_boundary", "player_pawn_event_vtable_protect_failed");
    return false;
  }
  g_player_pawn_event_original =
      reinterpret_cast<PlayerPawnEventFn>(expected);
  *slot = reinterpret_cast<void*>(&player_pawn_event_hook);
  VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
  g_player_pawn_event_vtable_slot = slot;
  g_player_pawn_event_hooked = true;
  log_kv("pov_boundary", "player_pawn_event_adapter_ok");
  return true;
}

bool __fastcall teamcounter_live_layout(void* /*game_rules*/) {
  return false;
}

bool install_teamcounter_live_layout(HMODULE client) {
  if (g_teamcounter_live_call_stub) {
    return true;
  }
  // This is the only TeamCounter override. False selects the game's native
  // live CT/T arrays: all ten avatars remain, while UpdatePlayer applies the
  // normal own-team health/C4/defuser and event-driven equipment timing rules.
  return install_rel_call_wrapper(
      client, offsets::kClientTeamCounterBroadcastCallRva,
      offsets::kClientTeamCounterBroadcastCallBytes,
      reinterpret_cast<const void*>(&teamcounter_live_layout),
      &g_teamcounter_live_call_addr, g_teamcounter_live_call_saved,
      &g_teamcounter_live_call_stub, "teamcounter_live_layout");
}

struct TeamCounterPlayerData {
  std::int32_t panel_index;
  std::int32_t player_id;
  std::uint32_t flags;
  std::int32_t health;
  std::int32_t armor;
};

void __fastcall teamcounter_player_data_filter(void* hud, void* raw_data) {
  const auto call = ++g_teamcounter_player_data_calls;
  if (raw_data && !demo_is_skipping()) {
    __try {
      HMODULE client = GetModuleHandleA(offsets::kClientName);
      auto* data = reinterpret_cast<TeamCounterPlayerData*>(raw_data);
      void* follow = follow_pawn_now(client);
      TeamCounterResolvePawnFn resolve = nullptr;
      if (client) {
        resolve = reinterpret_cast<TeamCounterResolvePawnFn>(
            reinterpret_cast<std::uint8_t*>(client) +
            offsets::kClientTeamCounterResolvePawnRva);
      }
      void* target = resolve ? resolve(data->player_id) : nullptr;
      const int follow_team = entity_team(follow);
      const int target_team = entity_team(target);
      const bool enemy = follow_team >= 2 && follow_team <= 3 &&
                         target_team >= 2 && target_team <= 3 &&
                         follow_team != target_team;
      const int old_health = data->health;
      const int old_armor = data->armor;
      const std::uint32_t old_flags = data->flags;
      std::uint64_t filtered = g_teamcounter_enemy_filters.load();
      if (enemy) {
        data->health = 0;
        data->armor = 0;
        data->flags &= ~((1u << 10) | (1u << 11));
        filtered = ++g_teamcounter_enemy_filters;
      }
      if (call <= 20 || (enemy && filtered <= 30)) {
        char detail[176]{};
        std::snprintf(detail, sizeof(detail),
                      "call=%llu id=%d team=%d/%d enemy=%d hp=%d->%d "
                      "armor=%d->%d flags=0x%X->0x%X",
                      static_cast<unsigned long long>(call), data->player_id,
                      follow_team, target_team, enemy ? 1 : 0, old_health,
                      data->health, old_armor, data->armor, old_flags,
                      data->flags);
        log_kv("teamcounter_player", detail);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ++g_teamcounter_filter_faults;
      if (call <= 40) {
        char detail[80]{};
        std::snprintf(detail, sizeof(detail), "seh code=0x%08lX",
                      static_cast<unsigned long>(GetExceptionCode()));
        log_kv("teamcounter_player", detail);
      }
    }
  }
  if (g_teamcounter_apply_player_data_original) {
    g_teamcounter_apply_player_data_original(hud, raw_data);
  }
}

bool install_teamcounter_player_data_filter(HMODULE client) {
  if (g_teamcounter_player_data_call_stub) {
    return true;
  }
  if (!client) {
    return false;
  }
  g_teamcounter_apply_player_data_original =
      reinterpret_cast<TeamCounterApplyPlayerDataFn>(
          reinterpret_cast<std::uint8_t*>(client) +
          offsets::kClientTeamCounterApplyPlayerDataRva);
  if (!install_rel_call_wrapper(
          client, offsets::kClientTeamCounterPlayerDataCallRva,
          offsets::kClientTeamCounterPlayerDataCallBytes,
          reinterpret_cast<const void*>(&teamcounter_player_data_filter),
          &g_teamcounter_player_data_call_addr,
          g_teamcounter_player_data_call_saved,
          &g_teamcounter_player_data_call_stub, "teamcounter_player_filter")) {
    g_teamcounter_apply_player_data_original = nullptr;
    return false;
  }
  return true;
}

std::int64_t __fastcall voice_packet_speaker_hook(void* voice_status,
                                                  unsigned ent,
                                                  int split_screen_slot,
                                                  std::uint8_t talking) {
  const auto calls = ++g_voice_packet_speakers;
  if (calls <= 16) {
    char msg[96]{};
    std::snprintf(msg, sizeof(msg),
                  "calls=%llu ent=%u ss=%d talking=%u",
                  static_cast<unsigned long long>(calls), ent,
                  split_screen_slot, static_cast<unsigned>(talking));
    log_kv("voice_packet", msg);
  }
  return g_voice_update_speaker_original
             ? g_voice_update_speaker_original(voice_status, ent,
                                                split_screen_slot, talking)
             : 0;
}

bool install_voice_packet_trace(HMODULE client) {
  if (g_voice_packet_call_stub) {
    return true;
  }
  if (!client) {
    return false;
  }
  g_voice_update_speaker_original =
      reinterpret_cast<VoiceUpdateSpeakerStatusFn>(
          reinterpret_cast<std::uint8_t*>(client) +
          offsets::kClientVoiceUpdateSpeakerStatusRva);
  if (!install_rel_call_wrapper(
          client, offsets::kClientVoicePacketSpeakerCallRva,
          offsets::kClientVoicePacketSpeakerCallBytes,
          reinterpret_cast<const void*>(&voice_packet_speaker_hook),
          &g_voice_packet_call_addr, g_voice_packet_call_saved,
          &g_voice_packet_call_stub, "voice_packet_trace")) {
    g_voice_update_speaker_original = nullptr;
    return false;
  }
  return true;
}

bool install_push_notice_hook(HMODULE client) {
  if (g_push_notice_hooked) {
    return true;
  }
  constexpr std::size_t kStolen = offsets::kClientPushNoticeStolen;
  g_push_notice_entry = reinterpret_cast<std::uint8_t*>(client) +
                        offsets::kClientPushNoticeRva;
  if (std::memcmp(g_push_notice_entry, offsets::kClientPushNoticePrologue,
                  sizeof(offsets::kClientPushNoticePrologue)) != 0) {
    log_kv("pipeline_patch", "push_notice_prologue_mismatch");
    return false;
  }
  std::memcpy(g_push_notice_stolen, g_push_notice_entry, kStolen);

  g_push_notice_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_push_notice_entry, 64));
  if (!g_push_notice_tramp_mem) {
    log_kv("pipeline_patch", "push_notice_tramp_alloc_failed");
    return false;
  }
  std::memcpy(g_push_notice_tramp_mem, g_push_notice_stolen, kStolen);
  if (!write_rel_jump(g_push_notice_tramp_mem + kStolen,
                      g_push_notice_entry + kStolen)) {
    VirtualFree(g_push_notice_tramp_mem, 0, MEM_RELEASE);
    g_push_notice_tramp_mem = nullptr;
    log_kv("pipeline_patch", "push_notice_tramp_back_oob");
    return false;
  }
  g_push_notice_tramp = reinterpret_cast<PushNoticeFn>(g_push_notice_tramp_mem);
  g_push_notice_hook_stub = g_push_notice_tramp_mem + 32;
  write_abs_jump(g_push_notice_hook_stub,
                 reinterpret_cast<const void*>(&push_notice_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_push_notice_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    VirtualFree(g_push_notice_tramp_mem, 0, MEM_RELEASE);
    g_push_notice_tramp_mem = nullptr;
    g_push_notice_hook_stub = nullptr;
    g_push_notice_tramp = nullptr;
    log_kv("pipeline_patch", "push_notice_protect_failed");
    return false;
  }
  if (!write_rel_jump(g_push_notice_entry, g_push_notice_hook_stub)) {
    VirtualProtect(g_push_notice_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_push_notice_tramp_mem, 0, MEM_RELEASE);
    g_push_notice_tramp_mem = nullptr;
    g_push_notice_hook_stub = nullptr;
    g_push_notice_tramp = nullptr;
    log_kv("pipeline_patch", "push_notice_hook_jmp_oob");
    return false;
  }
  for (std::size_t i = 5; i < kStolen; ++i) {
    g_push_notice_entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), g_push_notice_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_push_notice_tramp_mem, 64);
  VirtualProtect(g_push_notice_entry, kStolen, old_prot, &old_prot);

  g_push_notice_hooked = true;
  log_kv("pipeline_patch", want_pipeline()
                               ? "push_notice_passthrough_sink_ok"
                               : "push_notice_team_filter_ok");
  return true;
}

bool install_chat_notice_patches(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  static const std::uint8_t kNop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

  // Keep RadioText's demo/mute early-out intact. Re-enabling it produced the
  // unstyled "player threw grenade" lines seen in the last EWC test. The
  // weapon_fire listener below rebuilds only own-team grenade radio messages
  // with live-style team/name/location formatting.
  log_kv("pipeline_patch", "radio_mute_jne_skipped_throw_rebuild");

  g_chat_demo_jne_addr = base + offsets::kClientChatDemoJneRva;
  g_chat_demo_jne_patched = patch_bytes(
      g_chat_demo_jne_addr, offsets::kClientChatDemoJneBytes, 6, kNop6,
      g_chat_demo_jne_saved, "chat_demo_jne");

  g_saytext_demo_jne_addr = base + offsets::kClientSayTextDemoJneRva;
  g_saytext_demo_jne_patched = patch_bytes(
      g_saytext_demo_jne_addr, offsets::kClientSayTextDemoJneBytes, 6, kNop6,
      g_saytext_demo_jne_saved, "saytext_demo_jne");

  const bool notice = install_push_notice_hook(client);
  return g_chat_demo_jne_patched || g_saytext_demo_jne_patched || notice;
}

bool __fastcall voice_slot_team_filter(unsigned slot, bool speaking) {
  if (!speaking || demo_is_skipping()) {
    return false;
  }
  __try {
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    if (!client || notice_same_team_as_follow(client, slot)) {
      return true;
    }
    ++g_voice_team_drops;
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // This helper is reached through a generated CALL stub without unwind
    // metadata. Never let a stale demo entity exception escape across it.
    const auto faults = ++g_voice_filter_faults;
    if (faults <= 3) {
      log_kv("voice_filter", "team_lookup_seh fail_open=1");
    }
    return true;
  }
}

int __fastcall voice_mode_team_filter(unsigned slot, int native_mode) {
  const bool speaking = native_mode == 1 || native_mode == 2;
  return voice_slot_team_filter(slot, speaking) ? native_mode : 0;
}

bool __fastcall voice_speaking_team_filter(void* voice_state, unsigned slot) {
  if (!g_voice_speaking_original) {
    return false;
  }
  bool speaking = false;
  __try {
    speaking = g_voice_speaking_original(voice_state, slot);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_voice_runtime_disabled.store(true, std::memory_order_release);
    log_kv("voice_filter", "native_speaking_seh disabled=1");
    return false;
  }
  return voice_slot_team_filter(slot, speaking);
}

std::uint64_t __fastcall voice_status_update_hook(void* hud) {
  if (demo_is_skipping()) {
    ++g_voice_seek_skips;
    return 0;
  }
  if (g_voice_runtime_disabled.load(std::memory_order_acquire)) {
    return 0;
  }

  const std::uint64_t update = ++g_voice_updates;
  if (!hud || !g_voice_update_original) {
    if (update <= 3) {
      log_kv("voice_update", hud ? "native_missing" : "hud_missing");
    }
    return 0;
  }

  __try {
    // The native updater dereferences *(hud+248), then that object's +8 panel
    // interface, without null checks once a speaker record becomes active.
    // Demo startup can expose a recorded speaker one frame before Panorama has
    // finished binding VoicePanel. Defer that frame instead of crashing CS2.
    auto* panel_owner = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(hud) + 248);
    void* panel_interface = panel_owner
                                ? *reinterpret_cast<void**>(panel_owner + 8)
                                : nullptr;
    void** panel_vtable = panel_interface
                              ? *reinterpret_cast<void***>(panel_interface)
                              : nullptr;
    if (!panel_owner || !panel_interface || !panel_vtable) {
      if (update <= 3) {
        log_kv("voice_update", "panel_not_ready");
      }
      return 0;
    }
    if (update <= 3) {
      log_kv("voice_update", "panel_ready");
    }
    const std::uint64_t result = g_voice_update_original(hud);
    if (update <= 3) {
      log_kv("voice_update", "native_ok");
    }
    return result;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_voice_runtime_disabled.store(true, std::memory_order_release);
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "native_seh code=0x%08lX disabled=1",
                  static_cast<unsigned long>(GetExceptionCode()));
    log_kv("voice_update", msg);
    return 0;
  }
}

bool install_voice_status_patches(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  if (!g_voice_should_draw_patched) {
    g_voice_should_draw_addr = base + offsets::kClientVoiceShouldDrawRva;
    static const std::uint8_t kAlwaysDraw[3] = {0xB0, 0x01, 0xC3};
    g_voice_should_draw_patched = patch_bytes(
        g_voice_should_draw_addr, offsets::kClientVoiceShouldDrawBytes,
        sizeof(offsets::kClientVoiceShouldDrawBytes), kAlwaysDraw,
        g_voice_should_draw_saved, "voice_should_draw");
  }

  if (!g_voice_update_hooked) {
    g_voice_update_dispatch_addr =
        base + offsets::kClientVoiceUpdateDispatchRva;
    g_voice_update_original = reinterpret_cast<VoiceStatusUpdateFn>(
        base + offsets::kClientVoiceUpdateRva);
    auto* stub = reinterpret_cast<std::uint8_t*>(
        alloc_near(g_voice_update_dispatch_addr, 32));
    if (!stub) {
      log_kv("pipeline_patch", "voice_update_dispatch_alloc_failed");
    } else {
      write_abs_jump(stub, reinterpret_cast<const void*>(&voice_status_update_hook));
      FlushInstructionCache(GetCurrentProcess(), stub, sizeof(AbsJump));
      std::uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
      const std::intptr_t rel = stub - (g_voice_update_dispatch_addr + 5);
      if (rel > INT32_MAX || rel < INT32_MIN) {
        VirtualFree(stub, 0, MEM_RELEASE);
        log_kv("pipeline_patch", "voice_update_dispatch_jmp_oob");
      } else {
        *reinterpret_cast<std::int32_t*>(patch + 1) =
            static_cast<std::int32_t>(rel);
        if (patch_bytes(g_voice_update_dispatch_addr,
                        offsets::kClientVoiceUpdateDispatchBytes,
                        sizeof(offsets::kClientVoiceUpdateDispatchBytes), patch,
                        g_voice_update_dispatch_saved,
                        "voice_update_safe_dispatch")) {
          g_voice_update_dispatch_stub = stub;
          g_voice_runtime_disabled.store(false, std::memory_order_release);
          g_voice_update_hooked = true;
        } else {
          VirtualFree(stub, 0, MEM_RELEASE);
          g_voice_update_original = nullptr;
        }
      }
    }
  }

  if (!g_voice_team_filter_patched) {
    g_voice_mode_call_addr = base + offsets::kClientVoiceModeCallRva;
    g_voice_speaking_call_addr = base + offsets::kClientVoiceSpeakingCallRva;
    g_voice_mode_original = reinterpret_cast<VoiceModeFn>(
        base + offsets::kClientVoiceModeFnRva);
    g_voice_speaking_original = reinterpret_cast<VoiceSpeakingFn>(
        base + offsets::kClientVoiceSpeakingFnRva);

    // Recorded-player branch. Replace the existing game-mode CALL with a CALL
    // stub. Since this was already a call boundary, volatile GPR/XMM clobbering
    // is part of the compiler contract. r15d (slot) is nonvolatile and survives
    // the native mode query.
    auto* mode_stub = reinterpret_cast<std::uint8_t*>(
        alloc_near(g_voice_mode_call_addr, 64));
    if (!mode_stub) {
      log_kv("pipeline_patch", "voice_mode_filter_alloc_failed");
    } else {
      const std::uint8_t mode_code[] = {
          0x48, 0x83, 0xEC, 0x28,              // sub rsp,28h
          0x48, 0xB8,                          // mov rax,native_mode
          0,    0,    0,    0,    0, 0, 0, 0,
          0xFF, 0xD0,                          // call rax
          0x8B, 0xD0,                          // mov edx,eax
          0x41, 0x8B, 0xCF,                    // mov ecx,r15d
          0x48, 0xB8,                          // mov rax,filter
          0,    0,    0,    0,    0, 0, 0, 0,
          0xFF, 0xD0,                          // call rax
          0x48, 0x83, 0xC4, 0x28,              // add rsp,28h
          0xC3};                               // ret
      static_assert(sizeof(mode_code) == 38);
      std::memcpy(mode_stub, mode_code, sizeof(mode_code));
      *reinterpret_cast<std::uint64_t*>(mode_stub + 6) =
          reinterpret_cast<std::uint64_t>(g_voice_mode_original);
      *reinterpret_cast<std::uint64_t*>(mode_stub + 23) =
          reinterpret_cast<std::uint64_t>(&voice_mode_team_filter);
      FlushInstructionCache(GetCurrentProcess(), mode_stub, sizeof(mode_code));
      std::uint8_t mode_patch[5] = {0xE8, 0, 0, 0, 0};
      const std::intptr_t mode_rel =
          mode_stub - (g_voice_mode_call_addr + 5);
      if (mode_rel > INT32_MAX || mode_rel < INT32_MIN) {
        VirtualFree(mode_stub, 0, MEM_RELEASE);
        log_kv("pipeline_patch", "voice_mode_filter_call_oob");
      } else {
        *reinterpret_cast<std::int32_t*>(mode_patch + 1) =
            static_cast<std::int32_t>(mode_rel);
        if (patch_bytes(g_voice_mode_call_addr,
                        offsets::kClientVoiceModeCallBytes,
                        sizeof(offsets::kClientVoiceModeCallBytes), mode_patch,
                        g_voice_mode_call_saved, "voice_mode_team_filter")) {
          g_voice_mode_call_stub = mode_stub;
        } else {
          VirtualFree(mode_stub, 0, MEM_RELEASE);
        }
      }
    }

    // Live/no-record branch already passes (voice_state, slot) to a native
    // speaking predicate. Redirect that existing CALL to an ABI-identical
    // wrapper; the wrapper invokes the native predicate then applies team match.
    auto* speaking_stub = reinterpret_cast<std::uint8_t*>(
        alloc_near(g_voice_speaking_call_addr, 32));
    if (!speaking_stub) {
      log_kv("pipeline_patch", "voice_speaking_filter_alloc_failed");
    } else {
      write_abs_jump(speaking_stub,
                     reinterpret_cast<const void*>(&voice_speaking_team_filter));
      FlushInstructionCache(GetCurrentProcess(), speaking_stub, sizeof(AbsJump));
      std::uint8_t speaking_patch[5] = {0xE8, 0, 0, 0, 0};
      const std::intptr_t speaking_rel =
          speaking_stub - (g_voice_speaking_call_addr + 5);
      if (speaking_rel > INT32_MAX || speaking_rel < INT32_MIN) {
        VirtualFree(speaking_stub, 0, MEM_RELEASE);
        log_kv("pipeline_patch", "voice_speaking_filter_call_oob");
      } else {
        *reinterpret_cast<std::int32_t*>(speaking_patch + 1) =
            static_cast<std::int32_t>(speaking_rel);
        if (patch_bytes(g_voice_speaking_call_addr,
                        offsets::kClientVoiceSpeakingCallBytes,
                        sizeof(offsets::kClientVoiceSpeakingCallBytes),
                        speaking_patch, g_voice_speaking_call_saved,
                        "voice_speaking_team_filter")) {
          g_voice_speaking_call_stub = speaking_stub;
        } else {
          VirtualFree(speaking_stub, 0, MEM_RELEASE);
        }
      }
    }
    g_voice_team_filter_patched = g_voice_mode_call_stub != nullptr &&
                                  g_voice_speaking_call_stub != nullptr;
  }
  return g_voice_should_draw_patched && g_voice_update_hooked &&
         g_voice_team_filter_patched;
}

void __fastcall hud_money_update_hook(void* hud) {
  if (g_hud_money_runtime_disabled.load(std::memory_order_acquire)) {
    return;
  }
  const std::uint64_t update = ++g_hud_money_updates;
  if (!hud || !g_hud_money_original) {
    if (update <= 3) {
      log_kv("hud_money", hud ? "native_missing" : "hud_missing");
    }
    return;
  }
  __try {
    g_hud_money_original(hud);
    if (update <= 3) {
      log_kv("hud_money", "native_ok");
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_hud_money_runtime_disabled.store(true, std::memory_order_release);
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "native_seh code=0x%08lX disabled=1",
                  static_cast<unsigned long>(GetExceptionCode()));
    log_kv("hud_money", msg);
    return;
  }
  if (demo_is_skipping()) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return;
  }
  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    void* rules = *reinterpret_cast<void**>(base + offsets::kClientDwGameRules);
    void* follow = follow_pawn_now(client);
    bool show = false;
    if (rules && follow) {
      using RuleFn = bool(__fastcall*)(void*);
      const auto buy_state = reinterpret_cast<RuleFn>(
          base + offsets::kClientGameRulesBuyStateRva);
      const auto buy_time_elapsed = reinterpret_cast<RuleFn>(
          base + offsets::kClientBuyTimeElapsedRva);
      const bool in_zone = *reinterpret_cast<const std::uint8_t*>(
                               reinterpret_cast<const std::uint8_t*>(follow) +
                               offsets::kPawnInBuyZone) != 0;
      show = buy_state(rules) && !buy_time_elapsed(rules) && in_zone;
    }

    auto* hud_bytes = reinterpret_cast<std::uint8_t*>(hud);
    auto* panel = *reinterpret_cast<void**>(hud_bytes + 8);
    auto* cached = reinterpret_cast<std::uint8_t*>(hud_bytes + 40);
    if (panel && (*cached != static_cast<std::uint8_t>(show))) {
      using SetClassFn = void(__fastcall*)(void*, std::uint16_t, bool);
      auto** panel_vt = *reinterpret_cast<void***>(panel);
      auto set_class = reinterpret_cast<SetClassFn>(panel_vt[1280 / 8]);
      const std::uint16_t token = *reinterpret_cast<std::uint16_t*>(
          base + offsets::kClientHudMoneyInBuyZoneClassRva);
      set_class(panel, token, show);
      *cached = show ? 1 : 0;
      if (show) {
        ++g_cart_shown;
      } else {
        ++g_cart_hidden;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("hud_money", "strict_cart_seh");
  }
}

bool install_hud_money_hook(HMODULE client) {
  if (g_hud_money_hooked) {
    return true;
  }
  if (!client) {
    return false;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_hud_money_dispatch_addr =
      base + offsets::kClientHudMoneyUpdateDispatchRva;
  g_hud_money_original = reinterpret_cast<HudMoneyUpdateFn>(
      base + offsets::kClientHudMoneyUpdateRva);
  auto* stub = reinterpret_cast<std::uint8_t*>(
      alloc_near(g_hud_money_dispatch_addr, 32));
  if (!stub) {
    log_kv("pipeline_patch", "hud_money_dispatch_alloc_failed");
    g_hud_money_original = nullptr;
    return false;
  }
  write_abs_jump(stub, reinterpret_cast<const void*>(&hud_money_update_hook));
  FlushInstructionCache(GetCurrentProcess(), stub, sizeof(AbsJump));
  std::uint8_t patch[5] = {0xE9, 0, 0, 0, 0};
  const std::intptr_t rel = stub - (g_hud_money_dispatch_addr + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    VirtualFree(stub, 0, MEM_RELEASE);
    g_hud_money_original = nullptr;
    log_kv("pipeline_patch", "hud_money_dispatch_jmp_oob");
    return false;
  }
  *reinterpret_cast<std::int32_t*>(patch + 1) =
      static_cast<std::int32_t>(rel);
  if (!patch_bytes(g_hud_money_dispatch_addr,
                   offsets::kClientHudMoneyUpdateDispatchBytes,
                   sizeof(offsets::kClientHudMoneyUpdateDispatchBytes), patch,
                   g_hud_money_dispatch_saved,
                   "hud_money_safe_dispatch")) {
    VirtualFree(stub, 0, MEM_RELEASE);
    g_hud_money_original = nullptr;
    return false;
  }
  g_hud_money_dispatch_stub = stub;
  g_hud_money_runtime_disabled.store(false, std::memory_order_release);
  g_hud_money_hooked = true;
  return true;
}

bool install_kill_sound_patches(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_kill_cmp_addr = base + offsets::kClientKillSoundCmpJeRva;
  if (std::memcmp(g_kill_cmp_addr, offsets::kClientKillSoundCmpJeBytes,
                  sizeof(offsets::kClientKillSoundCmpJeBytes)) != 0) {
    log_kv("pipeline_patch", "kill_cmp_bytes_mismatch");
    return false;
  }

  // Replace `cmp rsi,rax; je deathcam` (5 bytes) with `call stub`.
  // Stub: observe (queue only) → restore rax → original cmp/je semantics.
  // Do NOT NOP deathcam cvar/mode gates; do NOT redirect deathcam.
  g_kill_match_stub =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_kill_cmp_addr, 80));
  if (!g_kill_match_stub) {
    log_kv("pipeline_patch", "kill_observe_stub_alloc_failed");
    return false;
  }

  std::uint8_t* s = g_kill_match_stub;
  // push rax          ; save local pawn from GetLocalPlayerPawn
  s[0] = 0x50;
  // sub rsp, 20h      ; shadow (rsp was 16-aligned after push)
  s[1] = 0x48;
  s[2] = 0x83;
  s[3] = 0xEC;
  s[4] = 0x20;
  // mov rcx, rsi
  s[5] = 0x48;
  s[6] = 0x89;
  s[7] = 0xF1;
  // mov rdx, rbx
  s[8] = 0x48;
  s[9] = 0x89;
  s[10] = 0xDA;
  // mov r8, r12
  s[11] = 0x4D;
  s[12] = 0x89;
  s[13] = 0xE0;
  // mov rax, &observe
  s[14] = 0x48;
  s[15] = 0xB8;
  *reinterpret_cast<std::uint64_t*>(s + 16) =
      reinterpret_cast<std::uint64_t>(&observe_player_death_kill_card);
  // call rax
  s[24] = 0xFF;
  s[25] = 0xD0;
  // add rsp, 20h
  s[26] = 0x48;
  s[27] = 0x83;
  s[28] = 0xC4;
  s[29] = 0x20;
  // pop rax
  s[30] = 0x58;
  // cmp rsi, rax
  s[31] = 0x48;
  s[32] = 0x3B;
  s[33] = 0xF0;
  // jne +6 → ret (not deathcam)
  s[34] = 0x75;
  s[35] = 0x06;
  // pop rcx          ; discard return addr (was C81E0C)
  s[36] = 0x59;
  // jmp deathcam C81E1C
  if (!write_rel_jump(s + 37, base + offsets::kClientKillSoundPlayRva)) {
    VirtualFree(g_kill_match_stub, 0, MEM_RELEASE);
    g_kill_match_stub = nullptr;
    log_kv("pipeline_patch", "kill_observe_deathcam_jmp_oob");
    return false;
  }
  // ret → C81E0C
  s[42] = 0xC3;
  g_kill_match_abs = nullptr;
  FlushInstructionCache(GetCurrentProcess(), g_kill_match_stub, 80);

  // call stub (E8) — same 5 bytes as original cmp/je
  std::uint8_t call_patch[5]{};
  call_patch[0] = 0xE8;
  {
    const std::intptr_t rel = g_kill_match_stub - (g_kill_cmp_addr + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) {
      VirtualFree(g_kill_match_stub, 0, MEM_RELEASE);
      g_kill_match_stub = nullptr;
      log_kv("pipeline_patch", "kill_observe_call_oob");
      return false;
    }
    *reinterpret_cast<std::int32_t*>(call_patch + 1) =
        static_cast<std::int32_t>(rel);
  }
  if (!patch_bytes(g_kill_cmp_addr, offsets::kClientKillSoundCmpJeBytes,
                   sizeof(offsets::kClientKillSoundCmpJeBytes), call_patch,
                   g_kill_cmp_saved, "kill_observe_call")) {
    VirtualFree(g_kill_match_stub, 0, MEM_RELEASE);
    g_kill_match_stub = nullptr;
    return false;
  }
  g_kill_cmp_patched = true;
  log_kv("pipeline_patch", "kill_observe_ok");
  return true;
}

void invalidate_radar_icon_styles(HMODULE client) {
  if (!client || !g_find_hud_element) {
    return;
  }
  __try {
    void* hud = g_find_hud_element("CCSGO_HudRadar");
    if (!hud) {
      return;
    }
    auto* base = reinterpret_cast<std::uint8_t*>(hud);
    const int last = *reinterpret_cast<int*>(base + offsets::kHudRadarPlayerLastIndex);
    if (last < 0 || last > 64) {
      return;
    }
    auto* arr = *reinterpret_cast<std::uint8_t**>(base + offsets::kHudRadarPlayerArray);
    if (!arr) {
      return;
    }
    for (int i = 0; i <= last; ++i) {
      auto* icon = arr + static_cast<std::uintptr_t>(i) *
                            offsets::kHudRadarPlayerStride;
      *reinterpret_cast<std::int32_t*>(icon + offsets::kHudRadarIconStyle) = -1;
    }
    ++g_icon_styles_invalidated;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void force_spectator_flash_opacity(HMODULE client) {
  float* cached = g_spectator_flash_opacity_value.load(std::memory_order_acquire);
  if (!cached && client) {
    auto* base = reinterpret_cast<std::uint8_t*>(client);
    __try {
      auto* obj = *reinterpret_cast<std::uint8_t**>(
          base + offsets::kClientSpectatorFlashOpacityCvarRva + 8);
      if (obj) {
        cached = reinterpret_cast<float*>(obj + 0x58);
        g_spectator_flash_opacity_value.store(cached, std::memory_order_release);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      cached = nullptr;
    }
  }
  if (!cached) {
    return;
  }
  __try {
    const float before = *cached;
    *cached = 1.f;
    static bool once = false;
    if (!once || before < 0.99f) {
      once = true;
      char msg[64]{};
      std::snprintf(msg, sizeof(msg), "spec_flash_opacity=%.2f->1", before);
      log_kv("flash", msg);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

BOOL CALLBACK enum_game_hwnd(HWND hwnd, LPARAM lparam) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) {
    return TRUE;
  }
  char title[192]{};
  if (GetWindowTextA(hwnd, title, sizeof(title)) <= 0) {
    return TRUE;
  }
  if (std::strstr(title, "Counter-Strike") == nullptr) {
    return TRUE;
  }
  *reinterpret_cast<HWND*>(lparam) = hwnd;
  return FALSE;
}

void destroy_flash_wash_overlay() {
  if (g_flash_wash_hwnd) {
    DestroyWindow(g_flash_wash_hwnd);
    g_flash_wash_hwnd = nullptr;
  }
  if (g_flash_wash_bmp) {
    DeleteObject(g_flash_wash_bmp);
    g_flash_wash_bmp = nullptr;
    g_flash_wash_bits = nullptr;
  }
  g_flash_wash_w = 0;
  g_flash_wash_h = 0;
  g_flash_wash_last_a = -1.f;
  g_game_hwnd = nullptr;
}

bool ensure_flash_wash_surface(int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (g_flash_wash_bmp && g_flash_wash_w == w && g_flash_wash_h == h) {
    return true;
  }
  if (g_flash_wash_bmp) {
    DeleteObject(g_flash_wash_bmp);
    g_flash_wash_bmp = nullptr;
    g_flash_wash_bits = nullptr;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    return false;
  }
  // Opaque white RGB; UpdateLayeredWindow supplies per-window alpha.
  auto* px = reinterpret_cast<std::uint32_t*>(bits);
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  for (std::size_t i = 0; i < n; ++i) {
    px[i] = 0x00FFFFFFu;
  }
  g_flash_wash_bmp = bmp;
  g_flash_wash_bits = bits;
  g_flash_wash_w = w;
  g_flash_wash_h = h;
  return true;
}

void update_flash_wash_overlay(float alpha) {
  if (alpha < 0.02f) {
    if (g_flash_wash_hwnd && IsWindowVisible(g_flash_wash_hwnd)) {
      ShowWindow(g_flash_wash_hwnd, SW_HIDE);
    }
    g_flash_wash_last_a = alpha;
    return;
  }
  if (!g_game_hwnd || !IsWindow(g_game_hwnd)) {
    g_game_hwnd = nullptr;
    EnumWindows(enum_game_hwnd, reinterpret_cast<LPARAM>(&g_game_hwnd));
    if (!g_game_hwnd) {
      return;
    }
  }
  RECT rc{};
  if (!GetClientRect(g_game_hwnd, &rc)) {
    return;
  }
  POINT tl{rc.left, rc.top};
  ClientToScreen(g_game_hwnd, &tl);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (!ensure_flash_wash_surface(w, h)) {
    return;
  }
  if (!g_flash_wash_hwnd) {
    g_flash_wash_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        L"Static", L"", WS_POPUP, tl.x, tl.y, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_flash_wash_hwnd) {
      return;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      log_kv("flash", "hud_wash_overlay_created");
    }
  }
  const BYTE a =
      static_cast<BYTE>(alpha >= 1.f ? 255 : static_cast<int>(alpha * 255.f));
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(mem, g_flash_wash_bmp);
  SIZE size{w, h};
  POINT src{0, 0};
  POINT dst{tl.x, tl.y};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = a;
  blend.AlphaFormat = 0;
  UpdateLayeredWindow(g_flash_wash_hwnd, screen, &dst, &size, mem, &src, 0,
                      &blend, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!IsWindowVisible(g_flash_wash_hwnd)) {
    ShowWindow(g_flash_wash_hwnd, SW_SHOWNOACTIVATE);
  }
  g_flash_wash_last_a = alpha;
}

// Drive HUD wash from world-overlay capture (high rate), not rare flash_hud
// binder calls. Stale captures (seq frozen) decay so we never stick at 1.0.
bool pump_flash_wash_overlay() {
  const std::uint32_t bits =
      g_view_flash.bits.load(std::memory_order_relaxed);
  const std::uint32_t seq =
      g_view_flash.seq.load(std::memory_order_relaxed);
  float view_a = 0.f;
  std::memcpy(&view_a, &bits, sizeof(view_a));

  static std::uint32_t s_last_seq = 0;
  static ULONGLONG s_last_change_ms = 0;
  static float s_display_a = 0.f;
  const ULONGLONG now = GetTickCount64();

  if (seq != s_last_seq) {
    s_last_seq = seq;
    s_last_change_ms = now;
    s_display_a = view_a;
  } else if (now - s_last_change_ms > 48) {
    // World overlay stopped writing — kill stuck full-white wash.
    s_display_a *= 0.65f;
    if (s_display_a < 0.03f) {
      s_display_a = 0.f;
      if (bits != 0) {
        g_view_flash.bits.store(0, std::memory_order_relaxed);
      }
    }
  } else {
    s_display_a = view_a;
  }

  update_flash_wash_overlay(s_display_a);
  return s_display_a > 0.02f;
}

void* remap_observer_pawn(void* pawn);  // defined below

bool install_icon_style_live_patches(HMODULE client) {
  if (g_icon_obs_jne_patched && g_icon_hltv_jne_patched &&
      g_icon_paint_obs_jne_patched && g_icon_paint_hltv_jne_patched) {
    return true;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  const std::uint8_t kNop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  const std::uint8_t kNop2[2] = {0x90, 0x90};

  g_icon_obs_jne_addr = base + offsets::kClientIconStyleObsJneRva;
  if (!g_icon_obs_jne_patched) {
    g_icon_obs_jne_patched = patch_bytes(
        g_icon_obs_jne_addr, offsets::kClientIconStyleObsJneBytes,
        sizeof(offsets::kClientIconStyleObsJneBytes), kNop6,
        g_icon_obs_jne_saved, "icon_style_obs");
  }

  g_icon_hltv_jne_addr = base + offsets::kClientIconStyleHltvJneRva;
  if (!g_icon_hltv_jne_patched) {
    g_icon_hltv_jne_patched = patch_bytes(
        g_icon_hltv_jne_addr, offsets::kClientIconStyleHltvJneBytes,
        sizeof(offsets::kClientIconStyleHltvJneBytes), kNop6,
        g_icon_hltv_jne_saved, "icon_style_hltv");
  }

  // Paint-flag path: or ebx,1 = demo numbers (independent of SetPlayerIconStyle).
  // Never NOP the style-unchanged je @ 0xE3AE9F — that always-calls apply(0)
  // and fades radar icons during freeze (frequent style re-evals).
  g_icon_paint_obs_jne_addr = base + offsets::kClientIconPaintObsJneRva;
  if (!g_icon_paint_obs_jne_patched) {
    g_icon_paint_obs_jne_patched = patch_bytes(
        g_icon_paint_obs_jne_addr, offsets::kClientIconPaintObsJneBytes,
        sizeof(offsets::kClientIconPaintObsJneBytes), kNop2,
        g_icon_paint_obs_jne_saved, "icon_paint_obs");
  }
  g_icon_paint_hltv_jne_addr = base + offsets::kClientIconPaintHltvJneRva;
  if (!g_icon_paint_hltv_jne_patched) {
    g_icon_paint_hltv_jne_patched = patch_bytes(
        g_icon_paint_hltv_jne_addr, offsets::kClientIconPaintHltvJneBytes,
        sizeof(offsets::kClientIconPaintHltvJneBytes), kNop2,
        g_icon_paint_hltv_jne_saved, "icon_paint_hltv");
  }

  return g_icon_obs_jne_patched && g_icon_hltv_jne_patched &&
         g_icon_paint_obs_jne_patched && g_icon_paint_hltv_jne_patched;
}

bool install_flash_spec_opacity_patch(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  // Replace call GetFloat + movaps xmm6,xmm0 (8 bytes) with movss xmm6,[1.0].
  g_flash_spec_opacity_addr = base + 0x11400D5;
  if (g_flash_spec_opacity_addr[0] != 0xE8 ||
      g_flash_spec_opacity_addr[5] != 0x0F ||
      g_flash_spec_opacity_addr[6] != 0x28 ||
      g_flash_spec_opacity_addr[7] != 0xF0) {
    log_kv("pipeline_patch", "flash_spec_opacity_bytes_mismatch");
    return false;
  }
  const auto* one = base + 0x19743D8;  // float 1.0 in this client build
  std::uint8_t patch[8] = {0xF3, 0x0F, 0x10, 0x35, 0, 0, 0, 0};
  const std::intptr_t disp =
      reinterpret_cast<const std::uint8_t*>(one) -
      (g_flash_spec_opacity_addr + 8);
  if (disp > INT32_MAX || disp < INT32_MIN) {
    log_kv("pipeline_patch", "flash_spec_opacity_disp_oob");
    return false;
  }
  *reinterpret_cast<std::int32_t*>(patch + 4) = static_cast<std::int32_t>(disp);
  std::uint8_t expect[8]{};
  std::memcpy(expect, g_flash_spec_opacity_addr, 8);
  g_flash_spec_opacity_patched =
      patch_bytes(g_flash_spec_opacity_addr, expect, 8, patch,
                  g_flash_spec_opacity_saved, "flash_spec_opacity");
  return g_flash_spec_opacity_patched;
}

bool install_flash_live_composite_patch(HMODULE client) {
  if (g_flash_live_composite_patched) {
    return true;
  }
  if (!client) {
    return false;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_flash_live_composite_addr =
      base + offsets::kClientFlashSpectatorCompositeTestRva;
  if (std::memcmp(g_flash_live_composite_addr,
                  offsets::kClientFlashSpectatorCompositeTestBytes,
                  sizeof(offsets::kClientFlashSpectatorCompositeTestBytes)) !=
      0) {
    char msg[80]{};
    std::snprintf(msg, sizeof(msg),
                  "live_composite_bytes_mismatch got=%02X%02X",
                  g_flash_live_composite_addr[0],
                  g_flash_live_composite_addr[1]);
    log_kv("flash_chain", msg);
    return false;
  }
  g_flash_live_composite_patched = patch_bytes(
      g_flash_live_composite_addr,
      offsets::kClientFlashSpectatorCompositeTestBytes,
      sizeof(offsets::kClientFlashSpectatorCompositeTestBytes),
      offsets::kClientFlashLiveCompositeBytes, g_flash_live_composite_saved,
      "flash_live_composite");
  if (g_flash_live_composite_patched) {
    log_kv("flash_chain",
           "live_render_order_ok spectator_panorama_flash_pass=skipped");
  }
  return g_flash_live_composite_patched;
}

// HudRadar UpdatePlayerIcon / icon style + TeamCounter call C112E0. Carpet
// remapping every C112E0 site (panorama IDs, demoui) cost ~50fps, hid teammate
// nametags, and crashed on seek via stale follow. Remap only HudRadar /
// TeamCounter RVA band (not demoui / panorama).
bool get_hud_alive_caller_ok(void* ret) {
  if (!ret) {
    return false;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return false;
  }
  const auto rva = static_cast<std::uint32_t>(
      reinterpret_cast<std::uint8_t*>(ret) -
      reinterpret_cast<std::uint8_t*>(client));
  // HudRadar + TeamCounter (Steam): E30000–E4A000 covers UpdatePlayerIcon,
  // icon style/paint, and money/roster — 21 C112E0 call sites. Broader carpet
  // (panorama / demoui) is what previously tanked FPS and seek.
  return rva >= 0xE30000 && rva < 0xE4A000;
}

bool follow_usable_for_hud(void* ent) {
  if (!ent) {
    return false;
  }
  __try {
    const auto team = *reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<std::uint8_t*>(ent) + offsets::kEntityTeamNum);
    return team == 2 || team == 3;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void log_alive_caller_rva(void* ret) {
  if (g_alive_rva_logs >= 24 || !ret) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return;
  }
  const auto rva = static_cast<std::uint32_t>(
      reinterpret_cast<std::uint8_t*>(ret) -
      reinterpret_cast<std::uint8_t*>(client));
  const bool ok = rva >= 0xE30000 && rva < 0xE4A000;
  char msg[64]{};
  std::snprintf(msg, sizeof(msg), "ret_rva=0x%X gate=%d", rva, ok ? 1 : 0);
  log_kv("alive_caller", msg);
  ++g_alive_rva_logs;
}

void* __fastcall get_hud_alive_pawn_hook() {
  ++g_hud_alive_calls;
  // Always prefer original during seek — entities are torn down.
  if (demo_is_skipping()) {
    return g_get_hud_alive_tramp ? g_get_hud_alive_tramp() : nullptr;
  }
  void* ret = _ReturnAddress();
  if (g_alive_rva_logs < 24) {
    log_alive_caller_rva(ret);
  }
  if (!get_hud_alive_caller_ok(ret)) {
    return g_get_hud_alive_tramp ? g_get_hud_alive_tramp() : nullptr;
  }
  ++g_hud_alive_gate_ok;

  HMODULE client = GetModuleHandleA(offsets::kClientName);
  // Do NOT call original C112E0 here: it applies IsAlive after slot→pawn and
  // drops demo chase targets (alive stayed 0/N with valid obs_h). Resolve
  // follow from ObserverTarget / cached pawn instead.
  void* follow = nullptr;
  __try {
    follow = follow_pawn_now(client);
    if (!follow) {
      void* local = g_slot_tramp ? g_slot_tramp(0) : nullptr;
      follow = remap_observer_pawn(local);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    follow = nullptr;
  }
  if (follow) {
    ++g_hud_alive_remaps;
    return follow;
  }
  ++g_hud_alive_no_follow;
  return nullptr;
}

bool install_get_hud_alive_hook(HMODULE client) {
  if (g_get_hud_alive_hooked) {
    return true;
  }
  constexpr std::size_t kStolen = offsets::kClientGetHudAlivePawnStolen;
  g_get_hud_alive_entry = reinterpret_cast<std::uint8_t*>(client) +
                          offsets::kClientGetHudAlivePawnRva;
  if (std::memcmp(g_get_hud_alive_entry, offsets::kClientGetHudAlivePawnPrologue,
                  kStolen) != 0) {
    log_kv("pipeline_patch", "get_hud_alive_prologue_mismatch");
    return false;
  }
  std::memcpy(g_get_hud_alive_stolen, g_get_hud_alive_entry, kStolen);

  g_get_hud_alive_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_get_hud_alive_entry, 64));
  if (!g_get_hud_alive_tramp_mem) {
    log_kv("pipeline_patch", "get_hud_alive_tramp_alloc_failed");
    return false;
  }
  std::memcpy(g_get_hud_alive_tramp_mem, g_get_hud_alive_stolen, kStolen);
  if (!write_rel_jump(g_get_hud_alive_tramp_mem + kStolen,
                      g_get_hud_alive_entry + kStolen)) {
    VirtualFree(g_get_hud_alive_tramp_mem, 0, MEM_RELEASE);
    g_get_hud_alive_tramp_mem = nullptr;
    log_kv("pipeline_patch", "get_hud_alive_tramp_back_oob");
    return false;
  }
  g_get_hud_alive_tramp =
      reinterpret_cast<GetHudPlayerFn>(g_get_hud_alive_tramp_mem);
  g_get_hud_alive_hook_stub = g_get_hud_alive_tramp_mem + 32;
  write_abs_jump(g_get_hud_alive_hook_stub,
                 reinterpret_cast<const void*>(&get_hud_alive_pawn_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_get_hud_alive_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    VirtualFree(g_get_hud_alive_tramp_mem, 0, MEM_RELEASE);
    g_get_hud_alive_tramp_mem = nullptr;
    g_get_hud_alive_hook_stub = nullptr;
    g_get_hud_alive_tramp = nullptr;
    log_kv("pipeline_patch", "get_hud_alive_protect_failed");
    return false;
  }
  if (!write_rel_jump(g_get_hud_alive_entry, g_get_hud_alive_hook_stub)) {
    VirtualProtect(g_get_hud_alive_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_get_hud_alive_tramp_mem, 0, MEM_RELEASE);
    g_get_hud_alive_tramp_mem = nullptr;
    g_get_hud_alive_hook_stub = nullptr;
    g_get_hud_alive_tramp = nullptr;
    log_kv("pipeline_patch", "get_hud_alive_hook_jmp_oob");
    return false;
  }
  for (std::size_t i = 5; i < kStolen; ++i) {
    g_get_hud_alive_entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), g_get_hud_alive_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_get_hud_alive_tramp_mem, 64);
  VirtualProtect(g_get_hud_alive_entry, kStolen, old_prot, &old_prot);

  g_get_hud_alive_hooked = true;
  log_kv("pipeline_patch", "get_hud_alive_remap_ok");
  return true;
}

bool install_grenade_pip_patch(HMODULE client) {
  if (g_grenade_pip_patched) {
    return true;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_grenade_pip_addr = base + offsets::kClientGrenadePipGateRva;
  // Entry has not pushed yet — single ret disables CGrenadeTracer PiP spawn.
  if (std::memcmp(g_grenade_pip_addr, offsets::kClientGrenadePipGatePrologue,
                  offsets::kClientGrenadePipGateStolen) != 0) {
    log_kv("pipeline_patch", "grenade_pip_bytes_mismatch");
    return false;
  }
  const std::uint8_t kRet[1] = {0xC3};
  const std::uint8_t expect1[1] = {offsets::kClientGrenadePipGatePrologue[0]};
  g_grenade_pip_patched =
      patch_bytes(g_grenade_pip_addr, expect1, 1, kRet, g_grenade_pip_saved,
                  "grenade_pip_ret");
  return g_grenade_pip_patched;
}

void restore_pipeline_code_patches();

bool install_pipeline_code_patches(HMODULE client) {
  // Pipeline V2 is expressed as native transaction scopes plus shared
  // identity/mode adapters. The only first-version component reachable here is
  // the user-approved, demo-data-only grenade notice exception below; all
  // other researched MVP surface patches remain dormant rollback/reference.
  log_kv("pipeline_flags",
         "v2_native_scopes=1 legacy_surface_patches=0 "
         "throw_adapter=v1_weapon_fire");
  bool installed = native_pipeline::install(client);
  if (installed && !install_player_pawn_event_hook(client)) {
    installed = false;
    log_kv("pov_pipeline", "player_pawn_event_failed_rollback");
    restore_pipeline_code_patches();
  }
  if (installed && env_flag_default_on("LIVE_HUD_VOICE_TRACE")) {
    if (install_voice_packet_trace(client)) {
      log_kv("pov_voice", "packet_to_speaking_trace_ok");
    } else {
      // Diagnostic-only: failure to observe this existing native CALL must not
      // disable the actual receive/decode/VoiceStatus pipeline.
      log_kv("pov_voice", "packet_to_speaking_trace_failed");
    }
  }
  if (installed && env_flag_default_on("LIVE_HUD_THROW_NOTICE")) {
    // This is the narrow, user-approved demo-data exception. Reuse the first
    // version's proven weapon_fire -> localized native notice path through the
    // already-installed Pawn event wrapper, while PushNotice itself remains a
    // transparent pass-through for all engine messages.
    installed = install_push_notice_hook(client);
    if (!installed) {
      log_kv("pov_compensation", "v1_throw_adapter_failed_rollback");
      restore_pipeline_code_patches();
    } else {
      log_kv("pov_compensation", "v1_throw_adapter_committed");
    }
  } else if (installed) {
    log_kv("pov_compensation", "throw_notice_disabled_env");
  }
  log_kv("pov_pipeline",
         installed ? "v2_transaction_committed"
                   : "v2_transaction_rejected");
  return installed;

  const bool chat_on = env_flag_default_on("LIVE_HUD_CHAT");
  // Stability A/B (2026-08-11): voice is opt-in again. It is the only new
  // subsystem whose first active frame consistently precedes the loading crash.
  const bool voice_on = env_flag_one("LIVE_HUD_VOICE");
  const bool cart_on = env_flag_default_on("LIVE_HUD_CART");
  const bool alive_on = env_flag_default_on("LIVE_HUD_HUD_ALIVE");
  const bool grenade_on = env_flag_default_on("LIVE_HUD_GRENADE_PIP");
  const bool ctrl_on = env_flag_default_on("LIVE_HUD_CONTROLLER_REMAP");
  const bool teamcounter_on =
      env_flag_default_on("LIVE_HUD_TEAMCOUNTER_LIVE");
  const bool throw_notice_on =
      env_flag_default_on("LIVE_HUD_THROW_NOTICE");
  const bool voice_trace_on =
      env_flag_default_on("LIVE_HUD_VOICE_TRACE");
  const bool flash_live_on =
      env_flag_default_on("LIVE_HUD_FLASH_LIVE_CHAIN");
  char flags[256]{};
  std::snprintf(flags, sizeof(flags),
                "chat=%d voice=%d voice_trace=%d cart=%d alive=%d grenade=%d "
                "ctrl=%d teamcounter=%d throw=%d kill=%d flash_live=%d",
                chat_on ? 1 : 0, voice_on ? 1 : 0,
                voice_trace_on ? 1 : 0, cart_on ? 1 : 0,
                alive_on ? 1 : 0, grenade_on ? 1 : 0, ctrl_on ? 1 : 0,
                teamcounter_on ? 1 : 0, throw_notice_on ? 1 : 0,
                env_flag_default_on("LIVE_HUD_KILL_SOUND") ? 1 : 0,
                flash_live_on ? 1 : 0);
  log_kv("pipeline_flags", flags);
  log_kv("pipeline_patch", "radar_enemy_hide_skipped");
  if (!install_icon_style_live_patches(client)) {
    log_kv("pipeline_patch", "icon_style_install_failed");
  }
  if (!flash_live_on) {
    log_kv("flash_chain", "live_render_order_disabled_env");
  } else if (!install_flash_live_composite_patch(client)) {
    log_kv("flash_chain", "live_render_order_install_failed");
  }
  if (!teamcounter_on) {
    log_kv("pipeline_patch", "teamcounter_live_layout_disabled_env");
  } else {
    if (!install_teamcounter_live_layout(client)) {
      log_kv("pipeline_patch", "teamcounter_live_layout_install_failed");
    }
    if (!install_teamcounter_player_data_filter(client)) {
      log_kv("pipeline_patch", "teamcounter_player_filter_install_failed");
    }
  }
  // Do not force r_spectator_flashbang_opacity and do not add a layered GDI
  // wash.  Both bypass the game's live render-order decision and make the
  // result look like an observer flash with HUD painted on top.
  log_kv("flash_chain", "legacy_spec_opacity_and_layered_wash=off");
  // Observe-only KillCard (preserves deathcam cmp). Set LIVE_HUD_KILL_SOUND=0 to disable.
  if (!env_flag_default_on("LIVE_HUD_KILL_SOUND")) {
    log_kv("pipeline_patch", "kill_sound_disabled_env");
  } else if (!install_kill_sound_patches(client)) {
    log_kv("pipeline_patch", "kill_sound_install_failed");
  }
  // Live-style bottom-left notices: own-team radios + hide enemy team-damage.
  if (!chat_on) {
    log_kv("pipeline_patch", "chat_notice_disabled_env");
  } else if (!install_chat_notice_patches(client)) {
    log_kv("pipeline_patch", "chat_notice_install_failed");
  }
  if (!throw_notice_on) {
    log_kv("pipeline_patch", "throw_notice_disabled_env");
  } else if (!chat_on) {
    log_kv("pipeline_patch", "throw_notice_skipped_chat_disabled");
  } else if (!install_player_pawn_event_hook(client)) {
    log_kv("pipeline_patch", "throw_notice_install_failed");
  }
  if (!voice_trace_on) {
    log_kv("pipeline_patch", "voice_packet_trace_disabled_env");
  } else if (!install_voice_packet_trace(client)) {
    log_kv("pipeline_patch", "voice_packet_trace_install_failed");
  }
  if (!voice_on) {
    log_kv("pipeline_patch", "voice_status_disabled_env");
  } else if (!install_voice_status_patches(client)) {
    log_kv("pipeline_patch", "voice_status_install_failed");
  }
  if (!cart_on) {
    log_kv("pipeline_patch", "hud_money_disabled_env");
  } else if (!install_hud_money_hook(client)) {
    log_kv("pipeline_patch", "hud_money_install_failed");
  }
  // HudRadar UpdatePlayerIcon calls C112E0; slot→pawn ret sits inside C112E0
  // so identity windows never remap. Allowlist-only + IsAlive (carpet remap
  // poisoned demoui / FPS / seek).
  if (!alive_on) {
    log_kv("pipeline_patch", "get_hud_alive_disabled_env");
  } else if (!install_get_hud_alive_hook(client)) {
    log_kv("pipeline_patch", "get_hud_alive_install_failed");
  }
  if (!grenade_on) {
    log_kv("pipeline_patch", "grenade_pip_disabled_env");
  } else if (!install_grenade_pip_patch(client)) {
    log_kv("pipeline_patch", "grenade_pip_install_failed");
  }
}

void restore_pipeline_code_patches() {
  native_pipeline::restore();
  if (g_player_pawn_event_hooked && g_player_pawn_event_vtable_slot) {
    DWORD old_prot = 0;
    if (VirtualProtect(g_player_pawn_event_vtable_slot, sizeof(void*),
                       PAGE_READWRITE, &old_prot)) {
      if (*g_player_pawn_event_vtable_slot ==
          reinterpret_cast<void*>(&player_pawn_event_hook)) {
        *g_player_pawn_event_vtable_slot =
            reinterpret_cast<void*>(g_player_pawn_event_original);
      }
      VirtualProtect(g_player_pawn_event_vtable_slot, sizeof(void*), old_prot,
                     &old_prot);
    }
  }
  g_player_pawn_event_hooked = false;
  g_player_pawn_event_vtable_slot = nullptr;
  g_player_pawn_event_original = nullptr;
  g_pawn_hurt_transactions.store(0, std::memory_order_relaxed);
  g_pawn_death_transactions.store(0, std::memory_order_relaxed);

  if (g_teamcounter_player_data_call_stub) {
    restore_bytes(g_teamcounter_player_data_call_addr,
                  g_teamcounter_player_data_call_saved,
                  sizeof(g_teamcounter_player_data_call_saved));
    VirtualFree(g_teamcounter_player_data_call_stub, 0, MEM_RELEASE);
  }
  g_teamcounter_player_data_call_addr = nullptr;
  g_teamcounter_player_data_call_stub = nullptr;
  g_teamcounter_apply_player_data_original = nullptr;

  if (g_teamcounter_live_call_stub) {
    restore_bytes(g_teamcounter_live_call_addr, g_teamcounter_live_call_saved,
                  sizeof(g_teamcounter_live_call_saved));
    VirtualFree(g_teamcounter_live_call_stub, 0, MEM_RELEASE);
  }
  g_teamcounter_live_call_addr = nullptr;
  g_teamcounter_live_call_stub = nullptr;

  if (g_voice_packet_call_stub) {
    restore_bytes(g_voice_packet_call_addr, g_voice_packet_call_saved,
                  sizeof(g_voice_packet_call_saved));
    VirtualFree(g_voice_packet_call_stub, 0, MEM_RELEASE);
  }
  g_voice_packet_call_addr = nullptr;
  g_voice_packet_call_stub = nullptr;
  g_voice_update_speaker_original = nullptr;

  if (g_voice_mode_call_stub) {
    restore_bytes(g_voice_mode_call_addr, g_voice_mode_call_saved,
                  sizeof(g_voice_mode_call_saved));
    VirtualFree(g_voice_mode_call_stub, 0, MEM_RELEASE);
  }
  if (g_voice_speaking_call_stub) {
    restore_bytes(g_voice_speaking_call_addr, g_voice_speaking_call_saved,
                  sizeof(g_voice_speaking_call_saved));
    VirtualFree(g_voice_speaking_call_stub, 0, MEM_RELEASE);
  }
  g_voice_mode_call_addr = nullptr;
  g_voice_mode_call_stub = nullptr;
  g_voice_mode_original = nullptr;
  g_voice_speaking_call_addr = nullptr;
  g_voice_speaking_call_stub = nullptr;
  g_voice_speaking_original = nullptr;
  g_voice_team_filter_patched = false;
  if (g_voice_update_hooked) {
    restore_bytes(g_voice_update_dispatch_addr, g_voice_update_dispatch_saved,
                  sizeof(g_voice_update_dispatch_saved));
    if (g_voice_update_dispatch_stub) {
      VirtualFree(g_voice_update_dispatch_stub, 0, MEM_RELEASE);
    }
    g_voice_update_dispatch_addr = nullptr;
    g_voice_update_dispatch_stub = nullptr;
    g_voice_update_original = nullptr;
    g_voice_runtime_disabled.store(false, std::memory_order_release);
    g_voice_update_hooked = false;
  }
  if (g_voice_should_draw_patched) {
    restore_bytes(g_voice_should_draw_addr, g_voice_should_draw_saved,
                  sizeof(g_voice_should_draw_saved));
    g_voice_should_draw_patched = false;
  }
  g_voice_should_draw_addr = nullptr;
  if (g_hud_money_hooked) {
    restore_bytes(g_hud_money_dispatch_addr, g_hud_money_dispatch_saved,
                  sizeof(g_hud_money_dispatch_saved));
    if (g_hud_money_dispatch_stub) {
      VirtualFree(g_hud_money_dispatch_stub, 0, MEM_RELEASE);
    }
    g_hud_money_dispatch_addr = nullptr;
    g_hud_money_dispatch_stub = nullptr;
    g_hud_money_original = nullptr;
    g_hud_money_runtime_disabled.store(false, std::memory_order_release);
    g_hud_money_hooked = false;
  }
  if (g_icon_obs_jne_patched) {
    restore_bytes(g_icon_obs_jne_addr, g_icon_obs_jne_saved,
                  sizeof(g_icon_obs_jne_saved));
    g_icon_obs_jne_patched = false;
  }
  if (g_icon_hltv_jne_patched) {
    restore_bytes(g_icon_hltv_jne_addr, g_icon_hltv_jne_saved,
                  sizeof(g_icon_hltv_jne_saved));
    g_icon_hltv_jne_patched = false;
  }
  if (g_icon_paint_obs_jne_patched) {
    restore_bytes(g_icon_paint_obs_jne_addr, g_icon_paint_obs_jne_saved,
                  sizeof(g_icon_paint_obs_jne_saved));
    g_icon_paint_obs_jne_patched = false;
  }
  if (g_icon_paint_hltv_jne_patched) {
    restore_bytes(g_icon_paint_hltv_jne_addr, g_icon_paint_hltv_jne_saved,
                  sizeof(g_icon_paint_hltv_jne_saved));
    g_icon_paint_hltv_jne_patched = false;
  }
  if (g_flash_spec_opacity_patched) {
    restore_bytes(g_flash_spec_opacity_addr, g_flash_spec_opacity_saved,
                  sizeof(g_flash_spec_opacity_saved));
    g_flash_spec_opacity_patched = false;
  }
  if (g_flash_live_composite_patched) {
    restore_bytes(g_flash_live_composite_addr, g_flash_live_composite_saved,
                  sizeof(g_flash_live_composite_saved));
    g_flash_live_composite_patched = false;
  }
  g_flash_live_composite_addr = nullptr;
  destroy_flash_wash_overlay();
  if (g_radar_enemy_hide_patched) {
    restore_bytes(g_radar_enemy_hide_addr, g_radar_enemy_hide_saved,
                  sizeof(g_radar_enemy_hide_saved));
    g_radar_enemy_hide_patched = false;
  }
  if (g_radar_enemy_hide_stub) {
    VirtualFree(g_radar_enemy_hide_stub, 0, MEM_RELEASE);
    g_radar_enemy_hide_stub = nullptr;
  }
  g_fow_suppress_left.store(0, std::memory_order_relaxed);
  if (g_kill_cmp_patched) {
    restore_bytes(g_kill_cmp_addr, g_kill_cmp_saved, sizeof(g_kill_cmp_saved));
    g_kill_cmp_patched = false;
  }
  if (g_kill_match_stub) {
    VirtualFree(g_kill_match_stub, 0, MEM_RELEASE);
    g_kill_match_stub = nullptr;
    g_kill_match_abs = nullptr;
  }
  if (g_kill_cvar_jne_patched) {
    restore_bytes(g_kill_cvar_jne_addr, g_kill_cvar_jne_saved,
                  sizeof(g_kill_cvar_jne_saved));
    g_kill_cvar_jne_patched = false;
  }
  if (g_kill_mode_jne_patched) {
    restore_bytes(g_kill_mode_jne_addr, g_kill_mode_jne_saved,
                  sizeof(g_kill_mode_jne_saved));
    g_kill_mode_jne_patched = false;
  }
  if (g_kill_mode_je_patched) {
    restore_bytes(g_kill_mode_je_addr, g_kill_mode_je_saved,
                  sizeof(g_kill_mode_je_saved));
    g_kill_mode_je_patched = false;
  }
  if (g_kill_fallback_je_patched) {
    restore_bytes(g_kill_fallback_je_addr, g_kill_fallback_je_saved,
                  sizeof(g_kill_fallback_je_saved));
    g_kill_fallback_je_patched = false;
  }
  if (g_radio_mute_jne_patched) {
    restore_bytes(g_radio_mute_jne_addr, g_radio_mute_jne_saved,
                  sizeof(g_radio_mute_jne_saved));
    g_radio_mute_jne_patched = false;
  }
  if (g_chat_demo_jne_patched) {
    restore_bytes(g_chat_demo_jne_addr, g_chat_demo_jne_saved,
                  sizeof(g_chat_demo_jne_saved));
    g_chat_demo_jne_patched = false;
  }
  if (g_saytext_demo_jne_patched) {
    restore_bytes(g_saytext_demo_jne_addr, g_saytext_demo_jne_saved,
                  sizeof(g_saytext_demo_jne_saved));
    g_saytext_demo_jne_patched = false;
  }
  if (g_push_notice_hooked && g_push_notice_entry) {
    constexpr std::size_t kStolen = offsets::kClientPushNoticeStolen;
    DWORD old_prot = 0;
    if (VirtualProtect(g_push_notice_entry, kStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_push_notice_entry, g_push_notice_stolen, kStolen);
      FlushInstructionCache(GetCurrentProcess(), g_push_notice_entry, kStolen);
      VirtualProtect(g_push_notice_entry, kStolen, old_prot, &old_prot);
    }
    if (g_push_notice_tramp_mem) {
      VirtualFree(g_push_notice_tramp_mem, 0, MEM_RELEASE);
      g_push_notice_tramp_mem = nullptr;
    }
    g_push_notice_hook_stub = nullptr;
    g_push_notice_tramp = nullptr;
    g_push_notice_entry = nullptr;
    g_push_notice_hooked = false;
  }
  if (g_get_hud_alive_hooked && g_get_hud_alive_entry) {
    constexpr std::size_t kStolen = offsets::kClientGetHudAlivePawnStolen;
    DWORD old_prot = 0;
    if (VirtualProtect(g_get_hud_alive_entry, kStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_get_hud_alive_entry, g_get_hud_alive_stolen, kStolen);
      FlushInstructionCache(GetCurrentProcess(), g_get_hud_alive_entry,
                            kStolen);
      VirtualProtect(g_get_hud_alive_entry, kStolen, old_prot, &old_prot);
    }
    if (g_get_hud_alive_tramp_mem) {
      VirtualFree(g_get_hud_alive_tramp_mem, 0, MEM_RELEASE);
      g_get_hud_alive_tramp_mem = nullptr;
    }
    g_get_hud_alive_hook_stub = nullptr;
    g_get_hud_alive_tramp = nullptr;
    g_get_hud_alive_entry = nullptr;
    g_get_hud_alive_hooked = false;
  }
  if (g_grenade_pip_patched) {
    restore_bytes(g_grenade_pip_addr, g_grenade_pip_saved, 1);
    g_grenade_pip_patched = false;
  }
}

void* remap_observer_pawn(void* pawn);  // defined below
void clear_teamcounter_money_sticky(HMODULE client);
void sync_flash_fields(void* dst_pawn, void* src_pawn);

void wipe_radar_after_team_edge(HMODULE client, void* follow_pawn) {
  // Do NOT FindHud / scrub icon arrays / call slot trampolines here or from
  // the watcher thread — that corrupted heap (last=garbage) and crashed.
  (void)client;
  g_fow_follow_pawn = follow_pawn;
  log_kv("radar_team_edge", "dirty_only");
}

void poll_radar_fow_work_impl() {
  // Intentionally empty: no game calls / entity walks from watcher thread.
}

void note_follow_pawn(void* pawn) {
  if (!pawn) {
    return;
  }
  const auto team = *reinterpret_cast<std::uint8_t*>(
      reinterpret_cast<std::uint8_t*>(pawn) + offsets::kEntityTeamNum);
  // CS teams: 2=T, 3=CT. Ignore unsettled 0/1.
  if (team != 2 && team != 3) {
    return;
  }
  const bool follow_changed = (g_fow_follow_pawn != pawn);
  g_fow_follow_pawn = pawn;
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  const auto published = pov::snapshot();
  if (follow_changed || published.pawn != pawn) {
    pov::Snapshot next{};
    next.pawn = pawn;
    next.controller = controller_from_pawn(client, pawn);
    next.team = team;
    // PawnGetPlayerSlot returns the caller-provided output pointer; the slot
    // itself is written through RDX. Treating RAX as the slot left RDX
    // uninitialized and caused the handled AV at client+0x90093F whenever a
    // demo acquired or changed its first-person target.
    using PawnSlotFn = int*(__fastcall*)(void*, int*);
    auto get_slot = client ? reinterpret_cast<PawnSlotFn>(
                                 reinterpret_cast<std::uint8_t*>(client) +
                                 offsets::kClientPawnGetPlayerSlotRva)
                           : nullptr;
    int slot = -1;
    __try {
      if (get_slot) {
        get_slot(pawn, &slot);
      }
      next.slot = slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      next.slot = -1;
    }
    pov::publish(next);
    log_kv("pov_identity", "follow_snapshot_published");
  }
  if (g_last_follow_team == team) {
    return;
  }
  const auto prev = g_last_follow_team;
  g_last_follow_team = team;
  if (prev != 0xFF) {
    char msg[48]{};
    std::snprintf(msg, sizeof(msg), "team %u->%u", prev, team);
    log_kv("radar_team_edge", msg);
    wipe_radar_after_team_edge(client, pawn);
    clear_teamcounter_money_sticky(client);
  }
}

void publish_follow_identity(HMODULE client, void* pawn) {
  // Intentionally empty: writing dwLocalPlayer* poisons demoui / spec_player
  // for the rest of the session. HUD paths use the remapped *return value*.
  (void)client;
  (void)pawn;
}

bool stack_in_hud_identity_window() {
  if (!hud_ranges_ready()) {
    return false;
  }
  // Keep shallow — identity remap fallback only; not on every IsHLTV tick.
  void* frames[8]{};
  const USHORT n = CaptureStackBackTrace(0, 8, frames, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    if (addr_in_hud_identity_window(
            reinterpret_cast<std::uintptr_t>(frames[i]))) {
      return true;
    }
  }
  return false;
}

void heal_local_globals(HMODULE client, void* tramp_pawn) {
  // No-op on the GetLocal hot path: we never publish follow into dwLocalPlayer*
  // anymore, so there is nothing to heal every call (was a large FPS cost).
  (void)client;
  (void)tramp_pawn;
}

// Remap to followed player. Prefer C_HLTVCamera primary (updates on demo
// spec_next / chase) when set; else ObserverTarget (set once user leaves
// empty cam for a player POV).
void* remap_observer_pawn(void* pawn) {
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return pawn;
  }
  // player_death pins the victim for the native red/death-banner window.  A
  // GOTV camera can switch its primary target to the killer immediately; do
  // not let that cut change slot->pawn identity midway through CA59C0 and turn
  // death_cam_phase1 into the unrelated phase2 black effect.
  if (pov::pinned()) {
    const auto latched = pov::snapshot();
    if (latched.pawn) {
      return latched.pawn;
    }
  }

  // Sample ObserverTarget for diag even when we prefer HLTV.
  if (pawn) {
    void* obs_svc = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(pawn) + offsets::kPawnObserverServices);
    if (obs_svc) {
      const auto h = *reinterpret_cast<std::uint32_t*>(
          reinterpret_cast<std::uint8_t*>(obs_svc) +
          offsets::kObserverTargetHandle);
      if (h != g_last_obs_handle && g_last_obs_handle != 0 &&
          g_last_obs_handle != 0xFFFFFFFEu) {
        // Follow target switched. Keep this deep GetLocal/identity hook free of
        // FindHud, icon-array writes, and MarkDirty re-entry. The always-live
        // style/paint patches will be used by the next native radar update.
        clear_teamcounter_money_sticky(client);
        log_kv("pov_identity", "follow_target_changed");
      }
      g_last_obs_handle = h;
    }
  }

  // Prefer HLTV chase target when the camera has a real primary index.
  if (void* hltv = hltv_primary_pawn(client)) {
    if (g_last_hltv_idx > 0) {
      note_follow_pawn(hltv);
      ++g_remap_hits;
      return hltv;
    }
  }

  // Allow cached-follow resolve when caller has no local pawn (C112E0 path).
  if (!pawn) {
    ++g_null_pawn;
    const auto target_handle = g_last_obs_handle;
    if (target_handle != 0 && target_handle != 0xFFFFFFFFu &&
        target_handle != 0xFFFFFFFEu) {
      if (void* target_pawn = entity_from_handle(client, target_handle)) {
        note_follow_pawn(target_pawn);
        ++g_remap_hits;
        return target_pawn;
      }
    }
    return nullptr;
  }

  void* obs_svc = *reinterpret_cast<void**>(
      reinterpret_cast<std::uint8_t*>(pawn) + offsets::kPawnObserverServices);
  if (obs_svc) {
    const auto target_handle = g_last_obs_handle;
    if (target_handle != 0 && target_handle != 0xFFFFFFFFu &&
        target_handle != 0xFFFFFFFEu) {
      if (void* target_pawn = entity_from_handle(client, target_handle)) {
        note_follow_pawn(target_pawn);
        ++g_remap_hits;
        return target_pawn;
      }
      ++g_no_target;
      note_fail_handle(target_handle);
    } else {
      ++g_bad_handle;
      note_fail_handle(target_handle);
    }
  } else {
    ++g_no_obs;
  }

  return pawn;
}

bool identity_hud_context(void* ret) {
  if (want_pipeline()) {
    return pov::active();
  }
  // Ret window first; shallow stack for HUD helpers (ret-only dropped remap
  // hit-rate ~35%→8% and the session felt like vanilla demo again).
  if (!hud_ranges_ready()) {
    return false;
  }
  if (ret && addr_in_hud_identity_window(
                 reinterpret_cast<std::uintptr_t>(ret))) {
    return true;
  }
  return stack_in_hud_identity_window();
}

bool controller_identity_hud_context(void* ret) {
  if (want_pipeline()) {
    return pov::active();
  }
  // Controller-returning helpers are also used as mere "local exists" gates.
  // A HUD frame deeper on the stack does not prove that the immediate consumer
  // can accept a different controller (B11860 is the concrete crash case).
  // Restrict controller substitution to direct, audited HUD callers. Pawn
  // substitution keeps the broader stack fallback in identity_hud_context().
  return hud_ranges_ready() && ret &&
         addr_in_hud_identity_window(reinterpret_cast<std::uintptr_t>(ret));
}

bool __fastcall is_observer_hook(void* pawn) {
  if (identity_hud_context(_ReturnAddress())) {
    ++g_isobs_lies;
    return false;
  }
  return g_isobs_tramp(pawn);
}

// TeamCounter money: bl = sub_85B6C0(local) || IsHLTVOrReplay.
// Spec/HLTV path always reveals economy; live uses freezetime timer when bl=0.
// Force false on TeamCounter money stacks so the live timer gate runs.
bool money_reveal_ret_in_teamcounter(void* ret) {
  if (!ret) {
    return false;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return false;
  }
  const auto rva = static_cast<std::uint32_t>(
      reinterpret_cast<std::uint8_t*>(ret) -
      reinterpret_cast<std::uint8_t*>(client));
  return rva >= 0xE47000 && rva < 0xE48000;
}

bool __fastcall money_reveal_hook(void* controller) {
  void* ret = _ReturnAddress();
  if (money_reveal_ret_in_teamcounter(ret)) {
    ++g_money_lies;
    return false;
  }
  // No CaptureStackBackTrace — TeamCounter calls are direct enough; stack
  // walks here showed up as sustained FPS loss.
  return g_money_tramp(controller);
}

void* game_rules(HMODULE client) {
  if (!client) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(client) +
                                   offsets::kClientDwGameRules);
}

bool read_freeze_period(HMODULE client) {
  void* gr = game_rules(client);
  if (!gr) {
    return false;
  }
  const auto* g = reinterpret_cast<const std::uint8_t*>(gr);
  // Prefer binder offset from this build; also accept schema 0x40.
  if (g[offsets::kGameRulesFreezePeriod]) {
    return true;
  }
  if (g[0x40]) {
    return true;
  }
  return false;
}

bool in_live_money_reveal_window() {
  // Freeze-period only (user request 2026-08-08): the top money strip should
  // behave like live freeze/buy time, not stay revealed for the whole round.
  // No post-freeze grace carryover — the freeze-end poll clears the sticky byte
  // so the strip drops immediately when the buy phase ends.
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  bool freeze = false;
  __try {
    freeze = read_freeze_period(client);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  g_last_freeze = freeze ? 1 : 0;
  return freeze;
}

void clear_teamcounter_money_sticky(HMODULE client) {
  // Retained as a compatibility call site only.  The old field was
  // misidentified and belongs to HudTeamCounter, not the HudMoney cart.
  (void)client;
}

// Live sticky gate should be freeze/reveal window — not demo's dword==2.
bool __fastcall money_sticky_gate_hook() {
  // Mid-seek the game tears down entities/gamerules; reading freeze state here
  // races that teardown (stale gamerules deref => AV). The strip is hidden
  // during a scrub anyway and is restyled on seek-end, so bail without touching
  // any game memory while skipping.
  if (demo_is_skipping()) {
    ++g_money_sticky_block;
    return false;
  }
  if (in_live_money_reveal_window()) {
    ++g_money_sticky_allow;
    return true;
  }
  ++g_money_sticky_block;
  return false;
}

void sync_flash_fields(void* dst_pawn, void* src_pawn) {
  if (!dst_pawn || !src_pawn || dst_pawn == src_pawn) {
    return;
  }
  __try {
    auto* d = reinterpret_cast<std::uint8_t*>(dst_pawn);
    auto* s = reinterpret_cast<std::uint8_t*>(src_pawn);
    // Flash block: overlay alpha / max / duration (schema ~0x1414–0x1428).
    // Always copy (including decay/clear) so HUD binders see the washout.
    constexpr std::uintptr_t kLo = 0x1414;
    constexpr std::uintptr_t kHi = 0x142C;
    std::memcpy(d + kLo, s + kLo, static_cast<std::size_t>(kHi - kLo));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Force follow pawn into Panorama HUD binders (flashed / health / etc.).
void* __fastcall flash_hud_player_hook() {
  ++g_flash_hud_hits;
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (demo_is_skipping()) {
    if (g_get_hud_player_tramp) {
      return g_get_hud_player_tramp();
    }
    if (g_slot_tramp) {
      return g_slot_tramp(0);
    }
    return nullptr;
  }
  if (!want_pipeline()) {
    pump_deferred_kill_card(client);
  }
  void* tramp = nullptr;
  if (g_slot_tramp) {
    tramp = g_slot_tramp(0);
  }
  void* follow = nullptr;
  __try {
    follow = remap_observer_pawn(tramp ? tramp : nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    follow = nullptr;
  }
  if (follow) {
    __try {
      auto* fp = reinterpret_cast<std::uint8_t*>(follow);
      const float overlay = *reinterpret_cast<float*>(fp + 0x141c);
      if (overlay > 0.02f) {
        static std::uint64_t s_flash_logs = 0;
        if (s_flash_logs < 32) {
          ++s_flash_logs;
          const float screenshot = *reinterpret_cast<float*>(fp + 0x1418);
          const float flash_end = *reinterpret_cast<float*>(fp + 0x1414);
          const float duration = *reinterpret_cast<float*>(fp + 0x1428);
          char msg[144]{};
          std::snprintf(msg, sizeof(msg),
                        "follow overlay=%.3f screenshot=%.3f end=%.3f "
                        "duration=%.3f source=native_pawn",
                        overlay, screenshot, flash_end, duration);
          log_kv("flash_chain", msg);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return follow;
  }
  if (g_get_hud_player_tramp) {
    return g_get_hud_player_tramp();
  }
  return tramp;
}

void* __fastcall slot_pawn_hook(int slot) {
  void* ret = _ReturnAddress();
  void* tramp_pawn = g_slot_tramp(slot);
  ++g_calls;
  if (slot != 0 && slot != -1) {
    return tramp_pawn;
  }
  // Seek tears down entities; never remap / heal / write spotted mid-skip.
  if (demo_is_skipping()) {
    if (pov::snapshot().pawn) {
      pov::invalidate();
    }
    return tramp_pawn;
  }
  HMODULE client_early = GetModuleHandleA(offsets::kClientName);
  if (!want_pipeline()) {
    pump_deferred_kill_card(client_early);
  }
  bool force_kill_remap = false;
  if (!want_pipeline() && ret) {
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    if (client) {
      const auto rva = static_cast<std::uint32_t>(
          reinterpret_cast<std::uint8_t*>(ret) -
          reinterpret_cast<std::uint8_t*>(client));
      // player_death kill-confirm @ 0xC81E02 — always remap here.
      if (rva >= 0xC81E00 && rva < 0xC82000) {
        force_kill_remap = true;
      }
    }
  }
  // Non-HUD: return real local. heal_local_globals is a no-op (we never publish
  // follow into dwLocalPlayer*); skip the call entirely on this hot path.
  if (!force_kill_remap && !identity_hud_context(ret)) {
    return tramp_pawn;
  }
  void* out = tramp_pawn;
  __try {
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    if (!want_pipeline() && client && ret) {
      const auto rva = static_cast<std::uint32_t>(
          reinterpret_cast<std::uint8_t*>(ret) -
          reinterpret_cast<std::uint8_t*>(client));
      // Count only the same direct player_death kill-confirm exception used
      // above; never treat the surrounding C8 event band as an identity gate.
      if (rva >= 0xC81E00 && rva < 0xC82000) {
        ++g_kill_gate_remaps;
      }
    }
    void* pawn = resolve_local_pawn(client, slot, tramp_pawn);
    out = remap_observer_pawn(pawn);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = tramp_pawn;
  }
  return out;
}

void* __fastcall slot_controller_hook(int slot) {
  void* ret = _ReturnAddress();
  void* tramp_ctrl = g_ctrl_tramp(slot);
  ++g_ctrl_calls;
  if (slot != 0 && slot != -1) {
    return tramp_ctrl;
  }
  if (demo_is_skipping()) {
    return tramp_ctrl;
  }
  // TeamCounter / many HUD panels use slot→controller, not pawn. Without this
  // remap the top strip stays on the HLTV spectator (all 10 players).
  if (!controller_identity_hud_context(ret)) {
    __try {
      HMODULE client = GetModuleHandleA(offsets::kClientName);
      if (client && ret) {
        const auto rva = static_cast<std::uint32_t>(
            reinterpret_cast<std::uint8_t*>(ret) -
            reinterpret_cast<std::uint8_t*>(client));
        if (rva >= offsets::kClientControllerExistsGuardLo &&
            rva < offsets::kClientControllerExistsGuardHi) {
          static std::atomic<std::uint32_t> s_guard_logs{0};
          if (s_guard_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
            char detail[80]{};
            std::snprintf(detail, sizeof(detail),
                          "passthrough local-exists rva=0x%X ctrl=%d", rva,
                          tramp_ctrl ? 1 : 0);
            log_kv("ctrl_guard", detail);
          }
        }
      }
      if (client && tramp_ctrl) {
        publish_local_controller(client, tramp_ctrl);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return tramp_ctrl;
  }
  void* out = tramp_ctrl;
  __try {
    HMODULE client = GetModuleHandleA(offsets::kClientName);
    void* pawn = pawn_from_controller(client, tramp_ctrl);
    void* follow = remap_observer_pawn(pawn);
    if (follow) {
      if (void* fc = controller_from_pawn(client, follow)) {
        ++g_ctrl_hits;
        out = fc;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out = tramp_ctrl;
  }
  return out;
}

// Resolve shared slot->pawn helper via GetLocal* wrapper's call.
std::uint8_t* find_slot_pawn_fn(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  const std::uint32_t size = module_size(client);
  const std::uint8_t pat[] = {
      0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC9, 0xE8,
      0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD8, 0x48,
      0x85, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x8B, 0x00, 0x48, 0x8B, 0xCB, 0xFF, 0x90,
      0xD8, 0x04, 0x00, 0x00};
  const char* mask = "xxxxxxxx????xxxxxxxx????xxxxxxxxxxxx";
  std::uint8_t* hit = find_pattern(base, size, pat, mask);
  if (!hit) {
    return nullptr;
  }
  const auto rel = *reinterpret_cast<std::int32_t*>(hit + 8);
  return hit + 12 + rel;
}

}  // namespace

bool want_pipeline() {
  return hlae_pipeline_requested() || env_flag_one("LIVE_HUD_PIPELINE");
}

void poll_radar_fow_work() { poll_radar_fow_work_impl(); }

void restyle_radar_icons(const char* reason) {
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client || !client_build_matches()) {
    return;
  }
  // Called by the watcher for seek/freeze edges. Do not call game HUD methods
  // or touch HudRadar's icon array from that thread. Those operations race the
  // Panorama/game update and previously produced delayed loading-stage crashes.
  // The installed branch patches are persistent; only the direct cvar value
  // needs maintenance here.
  if (!want_pipeline()) {
    force_teammate_colors_no_letters_client(client);
  }
  char msg[80]{};
  std::snprintf(msg, sizeof(msg), "%s_cvar_only",
                reason ? reason : "restyle");
  log_kv("radar", msg);
}

void restyle_radar_after_seek() {
  restyle_radar_icons("seek_end_restyle");
  if (want_pipeline()) {
    native_pipeline::note_seek_end();
  }
}

void poll_freeze_radar_restyle() {
  // Deliberate no-op. This runs on the watcher thread, where even read-only
  // GameRules/HUD access races demo loading and seeking. Radar style/paint is
  // now enforced only by permanent code patches plus direct ConVar storage.
}

void force_teammate_colors_no_letters() {
  // Resolve+write via cached/direct pointer only (no GetValue from watcher).
  if (!client_build_matches()) {
    return;
  }
  force_teammate_colors_no_letters_client(GetModuleHandleA(offsets::kClientName));
}

bool install_near_slot_hook(HMODULE client, std::uint32_t rva, void* hook_fn,
                            std::uint8_t** entry_out, std::uint8_t* stolen_out,
                            std::uint8_t** tramp_mem_out, std::uint8_t** stub_out,
                            SlotPawnFn* tramp_fn_out, const char* tag) {
  auto* entry = reinterpret_cast<std::uint8_t*>(client) + rva;
  static const std::uint8_t kExpect[kStolen] = {0x48, 0x83, 0xEC, 0x28,
                                               0x83, 0xF9, 0xFF};
  if (std::memcmp(entry, kExpect, kStolen) != 0) {
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_prologue_mismatch", tag);
    log_kv("identity", msg);
    return false;
  }
  std::memcpy(stolen_out, entry, kStolen);

  auto* mem = reinterpret_cast<std::uint8_t*>(alloc_near(entry, 64));
  if (!mem) {
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_tramp_alloc_failed", tag);
    log_kv("identity", msg);
    return false;
  }
  std::memcpy(mem, stolen_out, kStolen);
  if (!write_rel_jump(mem + kStolen, entry + kStolen)) {
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_tramp_back_oob", tag);
    log_kv("identity", msg);
    return false;
  }
  auto* stub = mem + 32;
  write_abs_jump(stub, hook_fn);

  // Publish the complete call-through path before the client entry can reach
  // the hook.  5E demos keep the client thread busy during installation and
  // exposed the old order: entry was patched first, slot_controller_hook ran,
  // and g_ctrl_tramp was still null (execute AV at address 0).
  *entry_out = entry;
  *tramp_mem_out = mem;
  *stub_out = stub;
  *tramp_fn_out = reinterpret_cast<SlotPawnFn>(mem);
  MemoryBarrier();

  DWORD old_prot = 0;
  if (!VirtualProtect(entry, kStolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
    *entry_out = nullptr;
    *tramp_mem_out = nullptr;
    *stub_out = nullptr;
    *tramp_fn_out = nullptr;
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_virtualprotect_failed", tag);
    log_kv("identity", msg);
    return false;
  }
  if (!write_rel_jump(entry, stub)) {
    VirtualProtect(entry, kStolen, old_prot, &old_prot);
    *entry_out = nullptr;
    *tramp_mem_out = nullptr;
    *stub_out = nullptr;
    *tramp_fn_out = nullptr;
    VirtualFree(mem, 0, MEM_RELEASE);
    char msg[64]{};
    std::snprintf(msg, sizeof(msg), "%s_hook_jmp_oob", tag);
    log_kv("identity", msg);
    return false;
  }
  entry[5] = 0x90;
  entry[6] = 0x90;
  FlushInstructionCache(GetCurrentProcess(), entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), mem, 64);
  VirtualProtect(entry, kStolen, old_prot, &old_prot);

  char msg[80]{};
  std::snprintf(msg, sizeof(msg), "%s_hooked rva=0x%X", tag, rva);
  log_kv("identity", msg);
  return true;
}

bool install_is_observer_hook(HMODULE client) {
  if (g_isobs_hooked) {
    return true;
  }
  constexpr std::size_t kStolen = offsets::kClientIsObserverOrDeadStolen;
  g_isobs_entry = reinterpret_cast<std::uint8_t*>(client) +
                  offsets::kClientIsObserverOrDeadRva;
  if (std::memcmp(g_isobs_entry, offsets::kClientIsObserverOrDeadPrologue,
                  sizeof(offsets::kClientIsObserverOrDeadPrologue)) != 0) {
    log_kv("identity", "isobs_prologue_mismatch");
    return false;
  }
  std::memcpy(g_isobs_stolen, g_isobs_entry, kStolen);

  // Stolen bytes have no RIP-rel; near page + E9 back is enough.
  g_isobs_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_isobs_entry, 64));
  if (!g_isobs_tramp_mem) {
    log_kv("identity", "isobs_tramp_alloc_failed");
    return false;
  }
  std::memcpy(g_isobs_tramp_mem, g_isobs_stolen, kStolen);
  if (!write_rel_jump(g_isobs_tramp_mem + kStolen, g_isobs_entry + kStolen)) {
    VirtualFree(g_isobs_tramp_mem, 0, MEM_RELEASE);
    g_isobs_tramp_mem = nullptr;
    log_kv("identity", "isobs_tramp_back_oob");
    return false;
  }
  g_isobs_tramp = reinterpret_cast<IsObserverFn>(g_isobs_tramp_mem);

  g_isobs_hook_stub = g_isobs_tramp_mem + 32;
  write_abs_jump(g_isobs_hook_stub,
                 reinterpret_cast<const void*>(&is_observer_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_isobs_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    VirtualFree(g_isobs_tramp_mem, 0, MEM_RELEASE);
    g_isobs_tramp_mem = nullptr;
    g_isobs_hook_stub = nullptr;
    g_isobs_tramp = nullptr;
    log_kv("identity", "isobs_virtualprotect_failed");
    return false;
  }
  if (!write_rel_jump(g_isobs_entry, g_isobs_hook_stub)) {
    VirtualProtect(g_isobs_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_isobs_tramp_mem, 0, MEM_RELEASE);
    g_isobs_tramp_mem = nullptr;
    g_isobs_hook_stub = nullptr;
    g_isobs_tramp = nullptr;
    log_kv("identity", "isobs_hook_jmp_oob");
    return false;
  }
  // Stolen=9; E9 is 5 bytes — pad remaining.
  for (std::size_t i = 5; i < kStolen; ++i) {
    g_isobs_entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), g_isobs_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_isobs_tramp_mem, 64);
  VirtualProtect(g_isobs_entry, kStolen, old_prot, &old_prot);

  g_isobs_hooked = true;
  log_kv("identity", "isobs_lie_hooked");
  return true;
}

bool install_money_reveal_hook(HMODULE client) {
  if (g_money_hooked) {
    return true;
  }
  constexpr std::size_t kStolen = offsets::kClientSpecMoneyRevealStolen;
  g_money_entry = reinterpret_cast<std::uint8_t*>(client) +
                  offsets::kClientSpecMoneyRevealRva;
  if (std::memcmp(g_money_entry, offsets::kClientSpecMoneyRevealPrologue,
                  sizeof(offsets::kClientSpecMoneyRevealPrologue)) != 0) {
    log_kv("identity", "money_prologue_mismatch");
    return false;
  }
  std::memcpy(g_money_stolen, g_money_entry, kStolen);

  g_money_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_money_entry, 64));
  if (!g_money_tramp_mem) {
    log_kv("identity", "money_tramp_alloc_failed");
    return false;
  }
  std::memcpy(g_money_tramp_mem, g_money_stolen, kStolen);
  if (!write_rel_jump(g_money_tramp_mem + kStolen, g_money_entry + kStolen)) {
    VirtualFree(g_money_tramp_mem, 0, MEM_RELEASE);
    g_money_tramp_mem = nullptr;
    log_kv("identity", "money_tramp_back_oob");
    return false;
  }
  g_money_tramp = reinterpret_cast<IsObserverFn>(g_money_tramp_mem);

  g_money_hook_stub = g_money_tramp_mem + 32;
  write_abs_jump(g_money_hook_stub,
                 reinterpret_cast<const void*>(&money_reveal_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_money_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    VirtualFree(g_money_tramp_mem, 0, MEM_RELEASE);
    g_money_tramp_mem = nullptr;
    g_money_hook_stub = nullptr;
    g_money_tramp = nullptr;
    log_kv("identity", "money_virtualprotect_failed");
    return false;
  }
  if (!write_rel_jump(g_money_entry, g_money_hook_stub)) {
    VirtualProtect(g_money_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_money_tramp_mem, 0, MEM_RELEASE);
    g_money_tramp_mem = nullptr;
    g_money_hook_stub = nullptr;
    g_money_tramp = nullptr;
    log_kv("identity", "money_hook_jmp_oob");
    return false;
  }
  for (std::size_t i = 5; i < kStolen; ++i) {
    g_money_entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), g_money_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_money_tramp_mem, 64);
  VirtualProtect(g_money_entry, kStolen, old_prot, &old_prot);

  g_money_hooked = true;
  log_kv("identity", "money_reveal_lie_hooked");
  return true;
}

bool install_money_sticky_gate_hook(HMODULE client) {
  if (g_money_sticky_hooked) {
    return true;
  }
  constexpr std::size_t kStolen = offsets::kClientMoneyStickyGateStolen;
  g_money_sticky_entry = reinterpret_cast<std::uint8_t*>(client) +
                         offsets::kClientMoneyStickyGateRva;
  if (std::memcmp(g_money_sticky_entry, offsets::kClientMoneyStickyGatePrologue,
                  sizeof(offsets::kClientMoneyStickyGatePrologue)) != 0) {
    log_kv("identity", "money_sticky_prologue_mismatch");
    return false;
  }
  std::memcpy(g_money_sticky_stolen, g_money_sticky_entry, kStolen);

  g_money_sticky_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_money_sticky_entry, 64));
  if (!g_money_sticky_tramp_mem) {
    log_kv("identity", "money_sticky_tramp_alloc_failed");
    return false;
  }
  std::memcpy(g_money_sticky_tramp_mem, g_money_sticky_stolen, kStolen);
  if (!write_rel_jump(g_money_sticky_tramp_mem + kStolen,
                      g_money_sticky_entry + kStolen)) {
    VirtualFree(g_money_sticky_tramp_mem, 0, MEM_RELEASE);
    g_money_sticky_tramp_mem = nullptr;
    log_kv("identity", "money_sticky_tramp_back_oob");
    return false;
  }
  g_money_sticky_tramp =
      reinterpret_cast<MoneyStickyFn>(g_money_sticky_tramp_mem);

  g_money_sticky_hook_stub = g_money_sticky_tramp_mem + 32;
  write_abs_jump(g_money_sticky_hook_stub,
                 reinterpret_cast<const void*>(&money_sticky_gate_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_money_sticky_entry, kStolen, PAGE_EXECUTE_READWRITE,
                      &old_prot)) {
    VirtualFree(g_money_sticky_tramp_mem, 0, MEM_RELEASE);
    g_money_sticky_tramp_mem = nullptr;
    g_money_sticky_hook_stub = nullptr;
    g_money_sticky_tramp = nullptr;
    log_kv("identity", "money_sticky_virtualprotect_failed");
    return false;
  }
  if (!write_rel_jump(g_money_sticky_entry, g_money_sticky_hook_stub)) {
    VirtualProtect(g_money_sticky_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_money_sticky_tramp_mem, 0, MEM_RELEASE);
    g_money_sticky_tramp_mem = nullptr;
    g_money_sticky_hook_stub = nullptr;
    g_money_sticky_tramp = nullptr;
    log_kv("identity", "money_sticky_hook_jmp_oob");
    return false;
  }
  for (std::size_t i = 5; i < kStolen; ++i) {
    g_money_sticky_entry[i] = 0x90;
  }
  FlushInstructionCache(GetCurrentProcess(), g_money_sticky_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_money_sticky_tramp_mem, 64);
  VirtualProtect(g_money_sticky_entry, kStolen, old_prot, &old_prot);

  g_money_sticky_hooked = true;
  log_kv("identity", "money_sticky_freeze_hooked");
  return true;
}

bool install_flash_hud_call_patches(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  const auto target = offsets::kClientGetHudPlayerRva;
  // Build a near abs-stub → flash_hud_player_hook once; retarget E8 sites.
  g_flash_hud_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(base + target, 64));
  if (!g_flash_hud_tramp_mem) {
    log_kv("identity", "flash_hud_stub_alloc_failed");
    return false;
  }
  g_flash_hud_hook_stub = g_flash_hud_tramp_mem;
  write_abs_jump(g_flash_hud_hook_stub,
                 reinterpret_cast<const void*>(&flash_hud_player_hook));
  // Keep original GetHudPlayer callable as fallback.
  g_get_hud_player_tramp =
      reinterpret_cast<GetHudPlayerFn>(base + target);

  int patched = 0;
  for (std::uint32_t rva = offsets::kClientFlashHudCallLo;
       rva + 5 <= offsets::kClientFlashHudCallHi; ++rva) {
    auto* p = base + rva;
    if (p[0] != 0xE8) {
      continue;
    }
    const auto rel = *reinterpret_cast<std::int32_t*>(p + 1);
    const auto dest =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + 5 + rel);
    if (dest != target) {
      continue;
    }
    const std::intptr_t new_rel =
        g_flash_hud_hook_stub - (p + 5);
    if (new_rel > INT32_MAX || new_rel < INT32_MIN) {
      continue;
    }
    DWORD old_prot = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old_prot)) {
      continue;
    }
    p[0] = 0xE8;
    *reinterpret_cast<std::int32_t*>(p + 1) =
        static_cast<std::int32_t>(new_rel);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    VirtualProtect(p, 5, old_prot, &old_prot);
    ++patched;
  }
  char msg[64]{};
  std::snprintf(msg, sizeof(msg), "flash_hud_calls_patched=%d", patched);
  log_kv("identity", msg);
  return patched > 0;
}

bool install_flash_amount_scale_patch(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_flash_scale_addr = base + offsets::kClientFlashAmountCvtRva;
  if (std::memcmp(g_flash_scale_addr, offsets::kClientFlashAmountCvtBytes,
                  sizeof(offsets::kClientFlashAmountCvtBytes)) != 0) {
    char msg[96]{};
    std::snprintf(
        msg, sizeof(msg),
        "flash_scale_bytes_mismatch got=%02X%02X%02X%02X%02X%02X%02X%02X",
        g_flash_scale_addr[0], g_flash_scale_addr[1], g_flash_scale_addr[2],
        g_flash_scale_addr[3], g_flash_scale_addr[4], g_flash_scale_addr[5],
        g_flash_scale_addr[6], g_flash_scale_addr[7]);
    log_kv("identity", msg);
    return false;
  }
  // Stub: movss xmm0,[rbx+0x141c]; mulss xmm0,[scale255]; cvttss2si edx,xmm0;
  //       clamp 0..255; jmp after_cvt
  g_flash_scale_stub =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_flash_scale_addr, 96));
  if (!g_flash_scale_stub) {
    log_kv("identity", "flash_scale_stub_alloc_failed");
    return false;
  }
  std::uint8_t* s = g_flash_scale_stub;
  // movss xmm0, [rbx+0x141c]
  s[0] = 0xF3;
  s[1] = 0x0F;
  s[2] = 0x10;
  s[3] = 0x83;
  s[4] = 0x1C;
  s[5] = 0x14;
  s[6] = 0x00;
  s[7] = 0x00;
  // push rax — after stub, CBFE1C does `mov rcx, rax` (Panorama prop handle).
  s[8] = 0x50;
  // mov rax, &g_view_flash.bits (seq lives at +4)
  s[9] = 0x48;
  s[10] = 0xB8;
  *reinterpret_cast<std::uint64_t*>(s + 11) =
      reinterpret_cast<std::uint64_t>(&g_view_flash.bits);
  // movss xmm1, [rax]
  s[19] = 0xF3;
  s[20] = 0x0F;
  s[21] = 0x10;
  s[22] = 0x08;
  // pop rax
  s[23] = 0x58;
  // maxss xmm0, xmm1
  s[24] = 0xF3;
  s[25] = 0x0F;
  s[26] = 0x5F;
  s[27] = 0xC1;
  // mulss xmm0, [rip+disp] → scale at 0x1975EFC
  s[28] = 0xF3;
  s[29] = 0x0F;
  s[30] = 0x59;
  s[31] = 0x05;
  {
    auto* imm = reinterpret_cast<std::int32_t*>(s + 32);
    const auto* scale = base + offsets::kClientFlashScale255Rva;
    *imm = static_cast<std::int32_t>(scale - (s + 36));
  }
  // cvttss2si edx, xmm0
  s[36] = 0xF3;
  s[37] = 0x0F;
  s[38] = 0x2C;
  s[39] = 0xD0;
  // test edx, edx / jns +2 / xor edx,edx
  s[40] = 0x85;
  s[41] = 0xD2;
  s[42] = 0x79;
  s[43] = 0x02;
  s[44] = 0x31;
  s[45] = 0xD2;
  // cmp edx, 0xff / jle +5 / mov edx, 0xff
  s[46] = 0x81;
  s[47] = 0xFA;
  s[48] = 0xFF;
  s[49] = 0x00;
  s[50] = 0x00;
  s[51] = 0x00;
  s[52] = 0x7E;
  s[53] = 0x05;
  s[54] = 0xBA;
  s[55] = 0xFF;
  s[56] = 0x00;
  s[57] = 0x00;
  s[58] = 0x00;
  if (!write_rel_jump(s + 59, base + offsets::kClientFlashAmountAfterCvtRva)) {
    VirtualFree(g_flash_scale_stub, 0, MEM_RELEASE);
    g_flash_scale_stub = nullptr;
    log_kv("identity", "flash_scale_jmp_oob");
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), g_flash_scale_stub, 96);

  // Replace 8-byte cvttss2si with E9 stub + 3 NOPs.
  std::uint8_t patch[8]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
  const std::intptr_t rel = g_flash_scale_stub - (g_flash_scale_addr + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    VirtualFree(g_flash_scale_stub, 0, MEM_RELEASE);
    g_flash_scale_stub = nullptr;
    log_kv("identity", "flash_scale_site_jmp_oob");
    return false;
  }
  *reinterpret_cast<std::int32_t*>(patch + 1) = static_cast<std::int32_t>(rel);
  if (!patch_bytes(g_flash_scale_addr, offsets::kClientFlashAmountCvtBytes,
                   sizeof(offsets::kClientFlashAmountCvtBytes), patch,
                   g_flash_scale_saved, "flash_scale")) {
    VirtualFree(g_flash_scale_stub, 0, MEM_RELEASE);
    g_flash_scale_stub = nullptr;
    return false;
  }
  g_flash_scale_patched = true;
  log_kv("identity", "flash_scale_ok");
  return true;
}

bool install_flash_overlay_capture_patch(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_flash_capture_addr = base + offsets::kClientFlashOverlayLoadRva;
  if (std::memcmp(g_flash_capture_addr, offsets::kClientFlashOverlayLoadBytes,
                  sizeof(offsets::kClientFlashOverlayLoadBytes)) != 0) {
    char msg[96]{};
    std::snprintf(
        msg, sizeof(msg),
        "flash_capture_bytes_mismatch got=%02X%02X%02X%02X%02X%02X%02X%02X",
        g_flash_capture_addr[0], g_flash_capture_addr[1],
        g_flash_capture_addr[2], g_flash_capture_addr[3],
        g_flash_capture_addr[4], g_flash_capture_addr[5],
        g_flash_capture_addr[6], g_flash_capture_addr[7]);
    log_kv("identity", msg);
    return false;
  }
  g_flash_capture_stub =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_flash_capture_addr, 80));
  if (!g_flash_capture_stub) {
    log_kv("identity", "flash_capture_stub_alloc_failed");
    return false;
  }
  std::uint8_t* s = g_flash_capture_stub;
  // movss xmm0, [rsi+0x141c]
  std::memcpy(s, offsets::kClientFlashOverlayLoadBytes, 8);
  // push rax (preserve — callers may still need it)
  s[8] = 0x50;
  // mov rax, &g_view_flash.bits (seq at +4)
  s[9] = 0x48;
  s[10] = 0xB8;
  *reinterpret_cast<std::uint64_t*>(s + 11) =
      reinterpret_cast<std::uint64_t>(&g_view_flash.bits);
  // movd [rax], xmm0
  s[19] = 0x66;
  s[20] = 0x0F;
  s[21] = 0x7E;
  s[22] = 0x00;
  // inc dword ptr [rax+4]  — freshness for HUD wash pump
  s[23] = 0xFF;
  s[24] = 0x40;
  s[25] = 0x04;
  // pop rax
  s[26] = 0x58;
  if (!write_rel_jump(s + 27, base + offsets::kClientFlashOverlayAfterLoadRva)) {
    VirtualFree(g_flash_capture_stub, 0, MEM_RELEASE);
    g_flash_capture_stub = nullptr;
    log_kv("identity", "flash_capture_jmp_oob");
    return false;
  }
  FlushInstructionCache(GetCurrentProcess(), g_flash_capture_stub, 80);

  std::uint8_t patch[8]{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
  const std::intptr_t rel = g_flash_capture_stub - (g_flash_capture_addr + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    VirtualFree(g_flash_capture_stub, 0, MEM_RELEASE);
    g_flash_capture_stub = nullptr;
    log_kv("identity", "flash_capture_site_jmp_oob");
    return false;
  }
  *reinterpret_cast<std::int32_t*>(patch + 1) = static_cast<std::int32_t>(rel);
  if (!patch_bytes(g_flash_capture_addr, offsets::kClientFlashOverlayLoadBytes,
                   sizeof(offsets::kClientFlashOverlayLoadBytes), patch,
                   g_flash_capture_saved, "flash_capture")) {
    VirtualFree(g_flash_capture_stub, 0, MEM_RELEASE);
    g_flash_capture_stub = nullptr;
    return false;
  }
  g_flash_capture_patched = true;
  log_kv("identity", "flash_capture_ok");
  return true;
}

bool install_local_identity_remap() {
  if (g_remap_hooked) {
    return true;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return false;
  }
  g_slot_entry = find_slot_pawn_fn(client);
  if (!g_slot_entry) {
    log_kv("identity", "slot_fn_not_found");
    return false;
  }

  static const std::uint8_t kExpect[kStolen] = {0x48, 0x83, 0xEC, 0x28,
                                               0x83, 0xF9, 0xFF};
  if (std::memcmp(g_slot_entry, kExpect, kStolen) != 0) {
    log_kv("identity", "prologue_mismatch");
    return false;
  }
  std::memcpy(g_slot_stolen, g_slot_entry, kStolen);

  // Decode entity chunk-table global from intact body:
  // 0x926DA2: 4C 8B 0D disp32  (mov r9, [rip+disp]) at entry+0x42.
  // 0x926D83: 4C 8D 05 disp32  (lea r8, [controller_table]) at entry+0x23.
  {
    auto* insn = g_slot_entry + 0x42;
    if (insn[0] == 0x4C && insn[1] == 0x8B && insn[2] == 0x0D) {
      const auto disp = *reinterpret_cast<std::int32_t*>(insn + 3);
      g_entity_chunk_table_ptr =
          reinterpret_cast<std::uintptr_t*>(insn + 7 + disp);
      char msg[64]{};
      std::snprintf(
          msg, sizeof(msg), "entity_table_rva=0x%X",
          static_cast<std::uint32_t>(
              reinterpret_cast<std::uint8_t*>(g_entity_chunk_table_ptr) -
              reinterpret_cast<std::uint8_t*>(client)));
      log_kv("identity", msg);
    } else {
      log_kv("identity", "entity_table_decode_failed");
    }
  }
  {
    auto* insn = g_slot_entry + 0x23;
    if (insn[0] == 0x4C && insn[1] == 0x8D && insn[2] == 0x05) {
      const auto disp = *reinterpret_cast<std::int32_t*>(insn + 3);
      g_controller_table =
          reinterpret_cast<std::uintptr_t*>(insn + 7 + disp);
      char msg[64]{};
      std::snprintf(
          msg, sizeof(msg), "controller_table_rva=0x%X",
          static_cast<std::uint32_t>(
              reinterpret_cast<std::uint8_t*>(g_controller_table) -
              reinterpret_cast<std::uint8_t*>(client)));
      log_kv("identity", msg);
    } else {
      // Fallback to schema absolute.
      g_controller_table = reinterpret_cast<std::uintptr_t*>(
          reinterpret_cast<std::uint8_t*>(client) +
          offsets::kClientDwLocalPlayerController);
      log_kv("identity", "controller_table_schema_fallback");
    }
  }

  // Near page layout:
  //   [0 .. kStolen+5)  trampoline (stolen + E9 back)
  //   [32 .. 32+12)     abs stub -> slot_pawn_hook (reachable when DLL is >2GB away)
  g_slot_tramp_mem =
      reinterpret_cast<std::uint8_t*>(alloc_near(g_slot_entry, 64));
  if (!g_slot_tramp_mem) {
    log_kv("identity", "tramp_alloc_near_failed");
    return false;
  }
  std::memcpy(g_slot_tramp_mem, g_slot_stolen, kStolen);
  if (!write_rel_jump(g_slot_tramp_mem + kStolen, g_slot_entry + kStolen)) {
    VirtualFree(g_slot_tramp_mem, 0, MEM_RELEASE);
    g_slot_tramp_mem = nullptr;
    log_kv("identity", "tramp_back_jmp_oob");
    return false;
  }
  g_slot_tramp = reinterpret_cast<SlotPawnFn>(g_slot_tramp_mem);

  g_hook_stub = g_slot_tramp_mem + 32;
  write_abs_jump(g_hook_stub, reinterpret_cast<const void*>(&slot_pawn_hook));

  DWORD old_prot = 0;
  if (!VirtualProtect(g_slot_entry, kStolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
    VirtualFree(g_slot_tramp_mem, 0, MEM_RELEASE);
    g_slot_tramp_mem = nullptr;
    g_hook_stub = nullptr;
    g_slot_tramp = nullptr;
    log_kv("identity", "virtualprotect_failed");
    return false;
  }
  // E9 to nearby abs-stub (always within ±2GB of client code).
  if (!write_rel_jump(g_slot_entry, g_hook_stub)) {
    VirtualProtect(g_slot_entry, kStolen, old_prot, &old_prot);
    VirtualFree(g_slot_tramp_mem, 0, MEM_RELEASE);
    g_slot_tramp_mem = nullptr;
    g_hook_stub = nullptr;
    g_slot_tramp = nullptr;
    log_kv("identity", "hook_jmp_oob");
    return false;
  }
  g_slot_entry[5] = 0x90;
  g_slot_entry[6] = 0x90;
  FlushInstructionCache(GetCurrentProcess(), g_slot_entry, kStolen);
  FlushInstructionCache(GetCurrentProcess(), g_slot_tramp_mem, 64);
  VirtualProtect(g_slot_entry, kStolen, old_prot, &old_prot);

  g_remap_hooked = true;
  const auto rva = static_cast<std::uint32_t>(
      g_slot_entry - reinterpret_cast<std::uint8_t*>(client));
  char msg[96]{};
  std::snprintf(msg, sizeof(msg),
                "pawn_remap_hooked rva=0x%X stolen=%zu via_abs_stub", rva,
                kStolen);
  log_kv("identity", msg);

  bool controller_ready = false;
  if (!want_pipeline() &&
      !env_flag_default_on("LIVE_HUD_CONTROLLER_REMAP")) {
    log_kv("identity", "ctrl_remap_disabled_env");
  } else {
    if (install_near_slot_hook(
            client, offsets::kClientSlotControllerRva,
            reinterpret_cast<void*>(&slot_controller_hook), &g_ctrl_entry,
            g_ctrl_stolen, &g_ctrl_tramp_mem, &g_ctrl_hook_stub, &g_ctrl_tramp,
            "ctrl_remap")) {
      g_ctrl_hooked = true;
      controller_ready = true;
    } else {
      log_kv("identity", "ctrl_install_failed_continuing");
    }
  }

  const bool observer_ready = install_is_observer_hook(client);
  if (!observer_ready) {
    log_kv("identity", "isobs_install_failed_continuing");
  }
  if (want_pipeline() && (!controller_ready || !observer_ready)) {
    log_kv("identity", "pov_identity_gate_failed_rollback");
    restore_local_identity_remap();
    return false;
  }
  // The former 0x85B6C0/0x863350 "money" hooks were TeamCounter/player-color
  // helpers, not the bottom-left cart.  Leave them uninstalled; HudMoney gets
  // its own strict buy-time + buy-zone hook in install_pipeline_code_patches.
  if (!want_pipeline() && !install_flash_hud_call_patches(client)) {
    log_kv("identity", "flash_hud_install_failed_continuing");
  }
  // `flashed` @ CC1054 is a TeamCounter status property, not the full-screen
  // HUD wash.  The 8A3B3C load feeds an animation parameter, not a view
  // compositor.  Leave both native instead of scaling/capturing them.
  log_kv("flash_chain", "teamcounter_scale=off animation_capture=off");
  if (!want_pipeline()) {
    force_teammate_colors_no_letters_client(client);
  }
  if (!install_pipeline_code_patches(client)) {
    log_kv("identity", "pov_transaction_failed_rollback");
    restore_local_identity_remap();
    return false;
  }
  return true;
}

void restore_local_identity_remap() {
  restore_pipeline_code_patches();

  // HLAE can toggle mirv_pov without unloading this DLL. Clear the watcher
  // one-shot state before any early return so the next enable can reinstall
  // the identity and native transactions.
  g_remap_done = false;
  g_early_radar_done = false;
  g_remap_pending_logged = false;

  if (g_flash_hud_tramp_mem) {
    // Call sites restored only on process exit; stub free is enough for unload.
    VirtualFree(g_flash_hud_tramp_mem, 0, MEM_RELEASE);
    g_flash_hud_tramp_mem = nullptr;
    g_flash_hud_hook_stub = nullptr;
    g_get_hud_player_tramp = nullptr;
  }
  if (g_flash_scale_patched) {
    restore_bytes(g_flash_scale_addr, g_flash_scale_saved,
                  sizeof(g_flash_scale_saved));
    g_flash_scale_patched = false;
  }
  if (g_flash_scale_stub) {
    VirtualFree(g_flash_scale_stub, 0, MEM_RELEASE);
    g_flash_scale_stub = nullptr;
  }
  if (g_flash_capture_patched) {
    restore_bytes(g_flash_capture_addr, g_flash_capture_saved,
                  sizeof(g_flash_capture_saved));
    g_flash_capture_patched = false;
  }
  if (g_flash_capture_stub) {
    VirtualFree(g_flash_capture_stub, 0, MEM_RELEASE);
    g_flash_capture_stub = nullptr;
  }
  if (g_money_sticky_hooked && g_money_sticky_entry) {
    constexpr std::size_t kStolen = offsets::kClientMoneyStickyGateStolen;
    DWORD old_prot = 0;
    if (VirtualProtect(g_money_sticky_entry, kStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_money_sticky_entry, g_money_sticky_stolen, kStolen);
      FlushInstructionCache(GetCurrentProcess(), g_money_sticky_entry, kStolen);
      VirtualProtect(g_money_sticky_entry, kStolen, old_prot, &old_prot);
    }
    if (g_money_sticky_tramp_mem) {
      VirtualFree(g_money_sticky_tramp_mem, 0, MEM_RELEASE);
      g_money_sticky_tramp_mem = nullptr;
    }
    g_money_sticky_hook_stub = nullptr;
    g_money_sticky_tramp = nullptr;
    g_money_sticky_entry = nullptr;
    g_money_sticky_hooked = false;
  }
  if (g_money_hooked && g_money_entry) {
    constexpr std::size_t kStolen = offsets::kClientSpecMoneyRevealStolen;
    DWORD old_prot = 0;
    if (VirtualProtect(g_money_entry, kStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_money_entry, g_money_stolen, kStolen);
      FlushInstructionCache(GetCurrentProcess(), g_money_entry, kStolen);
      VirtualProtect(g_money_entry, kStolen, old_prot, &old_prot);
    }
    if (g_money_tramp_mem) {
      VirtualFree(g_money_tramp_mem, 0, MEM_RELEASE);
      g_money_tramp_mem = nullptr;
    }
    g_money_hook_stub = nullptr;
    g_money_tramp = nullptr;
    g_money_entry = nullptr;
    g_money_hooked = false;
  }
  if (g_isobs_hooked && g_isobs_entry) {
    constexpr std::size_t kObsStolen = offsets::kClientIsObserverOrDeadStolen;
    DWORD old_prot = 0;
    if (VirtualProtect(g_isobs_entry, kObsStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_isobs_entry, g_isobs_stolen, kObsStolen);
      FlushInstructionCache(GetCurrentProcess(), g_isobs_entry, kObsStolen);
      VirtualProtect(g_isobs_entry, kObsStolen, old_prot, &old_prot);
    }
    if (g_isobs_tramp_mem) {
      VirtualFree(g_isobs_tramp_mem, 0, MEM_RELEASE);
      g_isobs_tramp_mem = nullptr;
    }
    g_isobs_hook_stub = nullptr;
    g_isobs_tramp = nullptr;
    g_isobs_entry = nullptr;
    g_isobs_hooked = false;
  }
  if (g_ctrl_hooked && g_ctrl_entry) {
    DWORD old_prot = 0;
    if (VirtualProtect(g_ctrl_entry, kStolen, PAGE_EXECUTE_READWRITE,
                       &old_prot)) {
      std::memcpy(g_ctrl_entry, g_ctrl_stolen, kStolen);
      FlushInstructionCache(GetCurrentProcess(), g_ctrl_entry, kStolen);
      VirtualProtect(g_ctrl_entry, kStolen, old_prot, &old_prot);
    }
    if (g_ctrl_tramp_mem) {
      VirtualFree(g_ctrl_tramp_mem, 0, MEM_RELEASE);
      g_ctrl_tramp_mem = nullptr;
    }
    g_ctrl_hook_stub = nullptr;
    g_ctrl_tramp = nullptr;
    g_ctrl_entry = nullptr;
    g_ctrl_hooked = false;
  }
  if (!g_remap_hooked || !g_slot_entry) {
    return;
  }
  DWORD old_prot = 0;
  if (VirtualProtect(g_slot_entry, kStolen, PAGE_EXECUTE_READWRITE, &old_prot)) {
    std::memcpy(g_slot_entry, g_slot_stolen, kStolen);
    FlushInstructionCache(GetCurrentProcess(), g_slot_entry, kStolen);
    VirtualProtect(g_slot_entry, kStolen, old_prot, &old_prot);
  }
  if (g_slot_tramp_mem) {
    VirtualFree(g_slot_tramp_mem, 0, MEM_RELEASE);
    g_slot_tramp_mem = nullptr;
  }
  g_hook_stub = nullptr;
  g_remap_hooked = false;
  g_slot_entry = nullptr;
  g_slot_tramp = nullptr;
}

void try_local_identity_remap_once() {
  if (!want_pipeline() || g_remap_done || g_remap_hooked) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    if (!g_remap_pending_logged) {
      log_kv("identity", "client_pending");
      g_remap_pending_logged = true;
    }
    return;
  }
  if (!client_build_matches()) {
    return;
  }

  // Hooking GetLocal during early client init has aborted launches. Wait until
  // the demo player reports playing (or LIVE_HUD_IDENTITY_EARLY=1 to force).
  if (!env_flag_one("LIVE_HUD_IDENTITY_EARLY")) {
    HMODULE engine2 = GetModuleHandleA(offsets::kEngine2Name);
    if (!engine2) {
      return;
    }
    const auto* demo = reinterpret_cast<const std::uint8_t*>(engine2) +
                       offsets::kEngine2DemoPlayerObjRva;
    if (demo[offsets::kDemoPlayerPlaying] == 0) {
      return;
    }
  }

  if (!install_local_identity_remap()) {
    log_kv("identity", "install_failed");
    // Retry next tick — do not permanently give up on transient failures.
    return;
  }
  g_remap_done = true;
  g_early_radar_done = true;
}

void try_early_icon_style_once() {
  if (want_pipeline() ||
      (g_icon_obs_jne_patched && g_icon_hltv_jne_patched)) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return;
  }
  if (!client_build_matches()) {
    return;
  }
  if (install_icon_style_live_patches(client)) {
    log_kv("identity", "early_icon_style_ok");
  }
}

// Before full identity (waits for playing=1): lie IsObserver + force colors so
// radar does not briefly show demo numbers/letters at demo start.
void try_early_radar_style_once() {
  if (want_pipeline() || g_early_radar_done) {
    return;
  }
  HMODULE client = GetModuleHandleA(offsets::kClientName);
  if (!client) {
    return;
  }
  if (!client_build_matches()) {
    return;
  }
  // Need HUD windows so isobs lie can match radar stacks.
  if (!hud_ranges_ready()) {
    return;
  }

  force_teammate_colors_no_letters_client(client);
  try_early_icon_style_once();

  if (!g_isobs_hooked) {
    if (!install_is_observer_hook(client)) {
      return;
    }
    log_kv("identity", "early_isobs_ok");
  }

  g_early_radar_done = true;
  log_kv("identity", "early_radar_style_ok");
}

void log_identity_diag_if_due() {
  if (!g_remap_hooked) {
    return;
  }
  const std::uint64_t calls = g_calls.load();
  if (calls == g_last_diag_calls) {
    return;
  }
  // Log on first activity and every ~256 calls thereafter.
  if (g_last_diag_calls != 0 && (calls - g_last_diag_calls) < 256) {
    return;
  }
  g_last_diag_calls = calls;
  if (g_last_obs_handle != g_last_logged_obs ||
      g_last_hltv_idx != g_last_logged_hltv) {
    char edge[80]{};
    std::snprintf(edge, sizeof(edge), "obs_h=0x%08X->0x%08X hltv_idx=%d->%d",
                  g_last_logged_obs, g_last_obs_handle, g_last_logged_hltv,
                  static_cast<int>(g_last_hltv_idx));
    log_kv("identity_edge", edge);
    g_last_logged_obs = g_last_obs_handle;
    g_last_logged_hltv = g_last_hltv_idx;
  }
  char msg[704]{};
  std::snprintf(
      msg, sizeof(msg),
      "calls=%llu hits=%llu ctrl=%llu/%llu null=%llu obs_fb=%llu hltv=%llu no_obs=%llu bad_h=%llu no_tgt=%llu isobs_lie=%llu money_lie=%llu money_fz=%llu/%llu flash_h=%llu alive=%llu/%llu gate=%llu miss=%llu kill_gate=%llu kill_m=%llu/%llu kill_card=%llu cash=%llu chat=%llu/%llu throw=%llu/%llu/%llu top=%llu/%llu/%llu voice_pkt=%llu voice=%llu skip=%llu drop=%llu cart=%llu/%llu last_h=0x%08X obs_h=0x%08X hltv_idx=%d",
      static_cast<unsigned long long>(calls),
      static_cast<unsigned long long>(g_remap_hits.load()),
      static_cast<unsigned long long>(g_ctrl_hits.load()),
      static_cast<unsigned long long>(g_ctrl_calls.load()),
      static_cast<unsigned long long>(g_null_pawn.load()),
      static_cast<unsigned long long>(g_obs_pawn_fallback.load()),
      static_cast<unsigned long long>(g_hltv_target_hits.load()),
      static_cast<unsigned long long>(g_no_obs.load()),
      static_cast<unsigned long long>(g_bad_handle.load()),
      static_cast<unsigned long long>(g_no_target.load()),
      static_cast<unsigned long long>(g_isobs_lies.load()),
      static_cast<unsigned long long>(g_money_lies.load()),
      static_cast<unsigned long long>(g_money_sticky_allow.load()),
      static_cast<unsigned long long>(g_money_sticky_block.load()),
      static_cast<unsigned long long>(g_flash_hud_hits.load()),
      static_cast<unsigned long long>(g_hud_alive_remaps.load()),
      static_cast<unsigned long long>(g_hud_alive_calls.load()),
      static_cast<unsigned long long>(g_hud_alive_gate_ok.load()),
      static_cast<unsigned long long>(g_hud_alive_no_follow.load()),
      static_cast<unsigned long long>(g_kill_gate_remaps.load()),
      static_cast<unsigned long long>(g_kill_match_hits.load()),
      static_cast<unsigned long long>(g_kill_match_checks.load()),
      static_cast<unsigned long long>(g_kill_card_plays.load()),
      static_cast<unsigned long long>(g_kill_cash_notices.load()),
      static_cast<unsigned long long>(g_chat_notice_pass.load()),
      static_cast<unsigned long long>(g_chat_notice_drop.load()),
      static_cast<unsigned long long>(g_throw_notice_shown.load()),
      static_cast<unsigned long long>(g_throw_event_calls.load()),
      static_cast<unsigned long long>(g_throw_notice_team_drops.load()),
      static_cast<unsigned long long>(g_teamcounter_enemy_filters.load()),
      static_cast<unsigned long long>(g_teamcounter_player_data_calls.load()),
      static_cast<unsigned long long>(g_teamcounter_filter_faults.load()),
      static_cast<unsigned long long>(g_voice_packet_speakers.load()),
      static_cast<unsigned long long>(g_voice_updates.load()),
      static_cast<unsigned long long>(g_voice_seek_skips.load()),
      static_cast<unsigned long long>(g_voice_team_drops.load()),
      static_cast<unsigned long long>(g_cart_shown.load()),
      static_cast<unsigned long long>(g_cart_hidden.load()),
      g_last_fail_handle, g_last_obs_handle,
      static_cast<int>(g_last_hltv_idx));
  log_kv("identity_diag", msg);
}

}  // namespace live_hud
