#include "MirvPovReferenceDetour.h"

#include "MirvPovReferenceCompat.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace live_hud::detour {
namespace {

#pragma pack(push, 1)
struct AbsJump {
  std::uint8_t mov_rax[2]{0x48, 0xB8};
  std::uint64_t target = 0;
  std::uint8_t jmp_rax[2]{0xFF, 0xE0};
};
#pragma pack(pop)

void* alloc_near(void* near_address, std::size_t size) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  std::uintptr_t granularity = info.dwAllocationGranularity;
  if (!granularity) {
    granularity = 0x10000;
  }
  const auto center = reinterpret_cast<std::uintptr_t>(near_address);
  const auto low =
      reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
  const auto high =
      reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
  constexpr std::uintptr_t kSpan = 0x70000000ull;

  auto try_address = [&](std::uintptr_t address) -> void* {
    if (address < low || address > high - size) {
      return nullptr;
    }
    address &= ~(granularity - 1);
    return VirtualAlloc(reinterpret_cast<void*>(address), size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  };

  for (std::uintptr_t delta = granularity; delta < kSpan;
       delta += granularity) {
    if (void* value = try_address(center + delta)) {
      return value;
    }
    if (center > delta) {
      if (void* value = try_address(center - delta)) {
        return value;
      }
    }
  }
  return nullptr;
}

bool write_rel_jump(std::uint8_t* from, const void* to) {
  const auto rel = reinterpret_cast<const std::uint8_t*>(to) - (from + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    return false;
  }
  from[0] = 0xE9;
  *reinterpret_cast<std::int32_t*>(from + 1) =
      static_cast<std::int32_t>(rel);
  return true;
}

void write_abs_jump(std::uint8_t* at, const void* to) {
  AbsJump jump{};
  jump.target = reinterpret_cast<std::uint64_t>(to);
  std::memcpy(at, &jump, sizeof(jump));
}

}  // namespace

bool install_entry(HMODULE module, std::uint32_t rva,
                   const std::uint8_t* expected, std::size_t stolen,
                   const void* replacement, EntryHook& hook,
                   const char* tag) {
  if (!module || !expected || !replacement || stolen < 5 || stolen > 16) {
    return false;
  }
  auto* entry = reinterpret_cast<std::uint8_t*>(module) + rva;
  if (std::memcmp(entry, expected, stolen) != 0) {
    char detail[96]{};
    std::snprintf(detail, sizeof(detail), "%s_prologue_mismatch", tag);
    log_kv("pov_boundary", detail);
    return false;
  }

  auto* allocation = reinterpret_cast<std::uint8_t*>(alloc_near(entry, 64));
  if (!allocation) {
    char detail[96]{};
    std::snprintf(detail, sizeof(detail), "%s_alloc_failed", tag);
    log_kv("pov_boundary", detail);
    return false;
  }

  std::memcpy(hook.saved, entry, stolen);
  std::memcpy(allocation, hook.saved, stolen);
  if (!write_rel_jump(allocation + stolen, entry + stolen)) {
    VirtualFree(allocation, 0, MEM_RELEASE);
    return false;
  }
  auto* stub = allocation + 32;
  write_abs_jump(stub, replacement);

  DWORD old_protect = 0;
  if (!VirtualProtect(entry, stolen, PAGE_EXECUTE_READWRITE, &old_protect)) {
    VirtualFree(allocation, 0, MEM_RELEASE);
    return false;
  }
  const bool jumped = write_rel_jump(entry, stub);
  if (jumped) {
    for (std::size_t index = 5; index < stolen; ++index) {
      entry[index] = 0x90;
    }
    FlushInstructionCache(GetCurrentProcess(), entry, stolen);
    FlushInstructionCache(GetCurrentProcess(), allocation, 64);
  }
  VirtualProtect(entry, stolen, old_protect, &old_protect);
  if (!jumped) {
    VirtualFree(allocation, 0, MEM_RELEASE);
    return false;
  }

  hook.entry = entry;
  hook.allocation = allocation;
  hook.stub = stub;
  hook.trampoline = allocation;
  hook.stolen = stolen;
  char detail[96]{};
  std::snprintf(detail, sizeof(detail), "%s_ok rva=0x%X", tag, rva);
  log_kv("pov_boundary", detail);
  return true;
}

void restore_entry(EntryHook& hook) noexcept {
  if (hook.entry && hook.stolen) {
    DWORD old_protect = 0;
    if (VirtualProtect(hook.entry, hook.stolen, PAGE_EXECUTE_READWRITE,
                       &old_protect)) {
      std::memcpy(hook.entry, hook.saved, hook.stolen);
      FlushInstructionCache(GetCurrentProcess(), hook.entry, hook.stolen);
      VirtualProtect(hook.entry, hook.stolen, old_protect, &old_protect);
    }
  }
  if (hook.allocation) {
    VirtualFree(hook.allocation, 0, MEM_RELEASE);
  }
  hook = {};
}

bool install_rel_call(HMODULE module, std::uint32_t call_rva,
                      const std::uint8_t expected[5],
                      const void* replacement, RelCallHook& hook,
                      const char* tag) {
  if (!module || !expected || !replacement) {
    return false;
  }
  auto* call = reinterpret_cast<std::uint8_t*>(module) + call_rva;
  if (std::memcmp(call, expected, 5) != 0) {
    char detail[96]{};
    std::snprintf(detail, sizeof(detail), "%s_bytes_mismatch", tag);
    log_kv("pov_boundary", detail);
    return false;
  }
  auto* stub = reinterpret_cast<std::uint8_t*>(alloc_near(call, 32));
  if (!stub) {
    char detail[96]{};
    std::snprintf(detail, sizeof(detail), "%s_alloc_failed", tag);
    log_kv("pov_boundary", detail);
    return false;
  }
  write_abs_jump(stub, replacement);
  const auto rel = stub - (call + 5);
  if (rel > INT32_MAX || rel < INT32_MIN) {
    VirtualFree(stub, 0, MEM_RELEASE);
    return false;
  }

  std::uint8_t patch[5]{0xE8, 0, 0, 0, 0};
  *reinterpret_cast<std::int32_t*>(patch + 1) =
      static_cast<std::int32_t>(rel);
  std::memcpy(hook.saved, call, 5);
  hook.call = call;
  hook.stub = stub;
  MemoryBarrier();

  DWORD old_protect = 0;
  if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
    hook = {};
    VirtualFree(stub, 0, MEM_RELEASE);
    return false;
  }
  std::memcpy(call, patch, 5);
  FlushInstructionCache(GetCurrentProcess(), call, 5);
  FlushInstructionCache(GetCurrentProcess(), stub, sizeof(AbsJump));
  VirtualProtect(call, 5, old_protect, &old_protect);
  char detail[96]{};
  std::snprintf(detail, sizeof(detail), "%s_ok rva=0x%X", tag, call_rva);
  log_kv("pov_boundary", detail);
  return true;
}

void restore_rel_call(RelCallHook& hook) noexcept {
  if (hook.call) {
    DWORD old_protect = 0;
    if (VirtualProtect(hook.call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
      bool owned = hook.call[0] == 0xE8;
      if (owned) {
        const auto rel = *reinterpret_cast<std::int32_t*>(hook.call + 1);
        owned = hook.call + 5 + rel == hook.stub;
      }
      if (owned) {
        std::memcpy(hook.call, hook.saved, 5);
        FlushInstructionCache(GetCurrentProcess(), hook.call, 5);
      }
      VirtualProtect(hook.call, 5, old_protect, &old_protect);
    }
  }
  if (hook.stub) {
    VirtualFree(hook.stub, 0, MEM_RELEASE);
  }
  hook = {};
}

}  // namespace live_hud::detour
