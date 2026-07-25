#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace amrvis {

// A 256-slot RGB palette compatible with the legacy X11 Amrvis sequential
// palette files (256 red bytes, then 256 green, then 256 blue, then an
// optional 256-byte alpha ramp, so 768 or 1024 bytes total; the alpha ramp
// is parsed but not used for 2-D rendering).
//
// Legacy index layout, matching Amrvis on a TrueColor display (Qt always
// renders as TrueColor): reserveSystemColors = 0, slot 0 is white, 1 is black,
// 2 is the body color, and data values map into [paletteStart, paletteEnd] =
// [3, 255]. Legacy reserved 24 system colors only on PseudoColor displays;
// doing that here skipped the dark-blue low slots and made the default
// colormap's blue too light. Reserved slots are kept exactly as stored in the
// file; data values never address them.
class Palette {
public:
    static constexpr int slotCount = 256;
    static constexpr int reservedSystemColors = 0;
    static constexpr int whiteIndex = reservedSystemColors;
    static constexpr int blackIndex = reservedSystemColors + 1;
    static constexpr int bodyIndex = reservedSystemColors + 2;
    static constexpr int paletteStart = reservedSystemColors + 3;
    static constexpr int paletteEnd = slotCount - 1;
    static constexpr int colorSlots = slotCount - reservedSystemColors - 3;

    struct Rgb {
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
    };

    Palette() = default;
    explicit Palette(const std::array<Rgb, slotCount>& slots);

    // Slot access; the index is clamped into [0, slotCount - 1].
    [[nodiscard]] const Rgb& slot(int index) const noexcept;
    // The slot as an opaque 0xAARRGGBB pixel.
    [[nodiscard]] std::uint32_t slotArgb(int index) const noexcept;

    // Maps a normalized value onto an opaque 0xAARRGGBB pixel using the
    // legacy AmrPicture semantics: t <= 0 selects paletteStart, t >= 1
    // selects paletteEnd, and in between the slot is
    // paletteStart + truncate(t * (colorSlots - 1)).  The legacy code
    // truncates into a slot instead of interpolating, so there is no
    // interpolating variant; NaN maps to paletteStart.
    [[nodiscard]] std::uint32_t argb(double t) const noexcept;

    // Approximates the legacy grid-level outline colors: level 0 (the
    // coarsest drawn level) is white, and finer levels follow
    // Palette::SafePaletteIndex across the palette's upper range, clamped
    // into [paletteStart, paletteEnd].
    [[nodiscard]] std::uint32_t levelColor(int level, int maxLevel) const noexcept;

    // Loads a legacy sequential palette file (768 or 1024 bytes).  Throws
    // std::runtime_error when the file cannot be read or has another size.
    [[nodiscard]] static Palette load(const std::filesystem::path& path);

private:
    std::array<Rgb, slotCount> slots_{};
};

enum class BuiltinPalette { Rainbow, Turbo, Viridis, Plasma, Parula, Coolwarm, Blackbody };

// Compiled-in copies of the palette files shipped under palettes/; Rainbow
// is byte-identical to the legacy default `Palette` file and is the default
// for scalar rendering. The rest are curated from the popular visualization
// packages: turbo, viridis, plasma (matplotlib), parula (MATLAB), coolwarm
// (the Moreland diverging map shared by ParaView/VisIt), and blackbody (a
// black-body radiation thermal ramp).
[[nodiscard]] const Palette& builtinPalette(BuiltinPalette palette);
// The palette's menu label / settings key, without a file extension, e.g.
// "rainbow".
[[nodiscard]] std::string_view builtinPaletteName(BuiltinPalette palette) noexcept;

} // namespace amrvis
