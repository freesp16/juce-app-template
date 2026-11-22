// X11 keysyms and modifier state → the keyboard map's codes (linux/KeyMap.h). Pure functions (no X connection).
// The keysym is the unshifted one of the key (XLookupKeysym index 0), so the map works on key positions (Shift+1 is
// still the "1" key).
#pragma once

namespace platform::linux_desktop
{
int keyCodeForKeysym(unsigned long keysym) noexcept;      // 0 = unmapped
unsigned modsForState(unsigned state) noexcept;           // X event state (ShiftMask, ControlMask, Mod1, Mod4) → kMod*
unsigned modifierForKeysym(unsigned long keysym) noexcept; // Shift_L → kModShift, …; 0 for other keys
} // namespace platform::linux_desktop
