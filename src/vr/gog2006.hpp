#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace sp::vr::gog2006 {

// June 19, 2006 GOG executable. Every hook also validates its own instructions.
inline bool active() noexcept
{
   static const bool result = [] {
      const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
      const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      const auto pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
      if (pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
          pe->FileHeader.TimeDateStamp != 0x449676b6 ||
          pe->OptionalHeader.SizeOfImage != 0x01bf3000) return false;
      const auto at = [base](std::uintptr_t va) { return base + va - 0x400000; };
      const std::uint8_t force[] = {0x56, 0x57, 0x8b, 0xf9, 0x33, 0xf6};
      return std::memcmp(at(0x4b2bf0), force, sizeof(force)) == 0 &&
             *reinterpret_cast<const std::uint32_t*>(at(0x7ca9fc)) ==
                reinterpret_cast<std::uintptr_t>(at(0x4b2bf0));
   }();
   return result;
}

inline std::uintptr_t address(std::uintptr_t va) noexcept
{
   return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + va - 0x400000;
}

// Only addresses independently located in the installed executable are mapped.
inline std::uintptr_t melee_address(std::uintptr_t va) noexcept
{
   if (!active()) return va;
   switch (va) {
   case 0x007e9ee0: return 0x007f9cd4; // CameraTracker array, stride 0x18
   case 0x0079d028: return 0x007ca9c0; // EntitySoldier +0x258
   case 0x004de390: return 0x004b2bf0; // trackable slot 15
   case 0x007b1578: return 0x007cf358; // WeaponMelee
   case 0x007b0ef4: return 0x007cee28; // WeaponGrenade
   case 0x007e6664: return 0x007fd11c; // shell manager
   case 0x00689290: return 0x00568d00; // WeaponMelee update (slot 1)
   case 0x0068a550: return 0x0056a030; // deflect (slot 18)
   case 0x00688b50: return 0x00568510; // deflecting (slot 20)
   case 0x0068c230: return 0x0056c0d0; // update fire (slot 41)
   case 0x0068a050: return 0x00569b10; // render (slot 35)
   case 0x0068bc00: return 0x0056bab0; // clear hits, ECX = weapon
   case 0x0068bef0: return 0x0056bda0; // leave state, ESI = weapon (LTCG)
   case 0x0068bc30: return 0x0056baf0; // enter state, EAX = state, ESI = weapon (LTCG)
   default: return 0; // unavailable features must not read a newer-build address
   }
}

} // namespace sp::vr::gog2006
