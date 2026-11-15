#pragma once

#include <cstdint>

namespace osx {

enum class ColormapId { ClassicSdrpp = 0, Turbo, Viridis, Inferno, Grayscale, Count };

const char* colormapName(ColormapId id);

// Fills 256 RGB8 entries (768 bytes), index 0 = weakest signal.
void buildColormap(ColormapId id, uint8_t* rgb);

}  // namespace osx
