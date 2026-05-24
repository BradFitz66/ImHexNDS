#pragma once

// Events shared between NDS plugins.
// Using EventManager with MinGW/GCC is safe across plugin DLLs because
// GCC uses string-based EventId hashing (FNV), not pointer-based typeid.

#include <hex/api/event_manager.hpp>
#include <map>
#include <string>

namespace hex {
    // Posted by PaletteViewer whenever its active palette offset changes.
    // Parameters: (u32 offset, bool active)
    //   offset — byte offset of the BGR555 palette in the open file
    //   active — false when the palette viewer has no file open
    EVENT_DEF_NO_LOG(EventNDSPaletteOffsetChanged, u32, bool);

    // Posted by PaletteViewer whenever its saved palette list changes.
    // Parameters: (const std::map<std::string, u32>& palettes)
    //   palettes — map of palette name → file offset of the 16-color BGR555 palette
    EVENT_DEF_NO_LOG(EventNDSSavedPalettesChanged, const std::map<std::string, u32>&);
}
