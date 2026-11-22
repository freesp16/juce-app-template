#include "linux/XKeyMap.h"

#include "linux/KeyMap.h"

#include <X11/X.h>
#include <X11/keysym.h>

namespace platform::linux_desktop
{
using namespace platform::host;

int keyCodeForKeysym(unsigned long ks) noexcept
{
    if (ks >= XK_F1 && ks <= XK_F12)
        return kKeyF1 + static_cast<int>(ks - XK_F1);
    switch (ks)
    {
    case XK_Left: case XK_KP_Left: return kKeyLeft;
    case XK_Right: case XK_KP_Right: return kKeyRight;
    case XK_Up: case XK_KP_Up: return kKeyUp;
    case XK_Down: case XK_KP_Down: return kKeyDown;
    case XK_Return: case XK_KP_Enter: return kKeyReturn;
    case XK_Escape: return kKeyEscape;
    case XK_Home: case XK_KP_Home: return kKeyHome;
    case XK_Delete: case XK_KP_Delete: return kKeyDelete;
    case XK_BackSpace: return kKeyBackspace;
    case XK_space: return ' ';
    default: break;
    }
    if (ks >= XK_A && ks <= XK_Z) // KeyMap normalises capitals too; keep the code lower-case
        return static_cast<int>(ks - XK_A) + 'a';
    if (ks >= 0x20 && ks < 0x7F) // Latin-1 keysyms of the printable ASCII range are their character
        return static_cast<int>(ks);
    return 0;
}

unsigned modsForState(unsigned state) noexcept
{
    return ((state & ShiftMask) != 0 ? kModShift : 0u) | ((state & ControlMask) != 0 ? kModCtrl : 0u)
         | ((state & Mod1Mask) != 0 ? kModAlt : 0u) | ((state & Mod4Mask) != 0 ? kModCmd : 0u);
}

unsigned modifierForKeysym(unsigned long ks) noexcept
{
    switch (ks)
    {
    case XK_Shift_L: case XK_Shift_R: return kModShift;
    case XK_Control_L: case XK_Control_R: return kModCtrl;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: return kModAlt;
    case XK_Super_L: case XK_Super_R: return kModCmd;
    default: return 0;
    }
}
} // namespace platform::linux_desktop
