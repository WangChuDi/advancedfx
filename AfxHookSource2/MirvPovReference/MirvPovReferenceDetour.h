#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace live_hud::detour {

struct EntryHook {
  std::uint8_t* entry = nullptr;
  std::uint8_t* allocation = nullptr;
  std::uint8_t* stub = nullptr;
  void* trampoline = nullptr;
  std::uint8_t saved[16]{};
  std::size_t stolen = 0;
};

struct RelCallHook {
  std::uint8_t* call = nullptr;
  std::uint8_t* stub = nullptr;
  std::uint8_t saved[5]{};
};

bool install_entry(HMODULE module, std::uint32_t rva,
                   const std::uint8_t* expected, std::size_t stolen,
                   const void* replacement, EntryHook& hook,
                   const char* tag);
void restore_entry(EntryHook& hook) noexcept;

bool install_rel_call(HMODULE module, std::uint32_t call_rva,
                      const std::uint8_t expected[5],
                      const void* replacement, RelCallHook& hook,
                      const char* tag);
void restore_rel_call(RelCallHook& hook) noexcept;

}  // namespace live_hud::detour
