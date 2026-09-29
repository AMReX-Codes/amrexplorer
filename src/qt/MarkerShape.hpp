#pragma once

#include <array>

namespace amrvis::qt {

// How a particle species is drawn. Circle is the round dot every species used
// before shapes existed.
enum class MarkerShape { Circle, Square, Diamond, Triangle, Cross };

inline constexpr std::array<MarkerShape, 5> markerShapes{MarkerShape::Circle,
    MarkerShape::Square, MarkerShape::Diamond, MarkerShape::Triangle,
    MarkerShape::Cross};

} // namespace amrvis::qt
