#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace amrvis {

// A 256-slot RGB palette compatible with the legacy X11 Amrvis sequential
// palette files (256 red bytes, then 256 green, then 256 blue, then an
// optional 256-byte alpha ramp, so 768 or 1024 bytes total). The alpha ramp
// is the legacy volume-rendering transfer function -- one opacity per slot,
// stored as a percentage -- and is kept when present (hasAlphaRamp,
// opacity); 2-D rendering ignores it.
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
        friend bool operator==(const Rgb&, const Rgb&) = default;
    };

    Palette() = default;
    explicit Palette(const std::array<Rgb, slotCount>& slots,
        const std::optional<std::array<std::uint8_t, slotCount>>& alpha
        = std::nullopt);

    // Slot access; the index is clamped into [0, slotCount - 1].
    [[nodiscard]] const Rgb& slot(int index) const noexcept;
    // The slot as an opaque 0xAARRGGBB pixel.
    [[nodiscard]] std::uint32_t slotArgb(int index) const noexcept;

    // Whether the palette carried a usable alpha ramp: a 1024-byte file's
    // fourth plane (the builtins carry their .pal file's), unless it is all
    // zero -- a plane that would make every value invisible is treated as
    // absent rather than authored.
    [[nodiscard]] bool hasAlphaRamp() const noexcept;
    // The slot's opacity in [0, 1] for volume rendering. A ramp is read with
    // the legacy Amrvis semantics -- each byte a percentage -- unless any
    // data slot's byte exceeds 100, which no Amrvis-written ramp does: such
    // a ramp is read as 0..255 (a full-scale byte ramp, and the flat 255
    // that older AMReXplorer builds wrote, which then reads as opaque rather
    // than as 255 %). The format has no version marker, so this sniff is
    // the whole rule; its one blind spot is a genuinely faint full-scale
    // ramp that never exceeds byte 100, which reads as percent and comes out
    // brighter than authored. Only the data slots [paletteStart, paletteEnd]
    // are inspected: the reserved slots are kept as stored and never
    // addressed by data. A palette without a ramp uses Amrvis's default
    // transfer function, the linear ramp index / (slotCount - 1). The index
    // is clamped into [0, slotCount - 1].
    [[nodiscard]] double opacity(int index) const noexcept;

    // A copy with the data color range [paletteStart, paletteEnd] reversed,
    // leaving the reserved slots (white/black/body) untouched. The result maps
    // values to colors in the opposite order -- the "_r" variant of the
    // palette (e.g. plasma_r) -- so argb, the color bar, slotArgb over the data
    // range, and levelColor all follow the reversed ramp. The alpha ramp is
    // left as stored: opacity is a function of the data value, not of the
    // color drawn for it.
    [[nodiscard]] Palette reversed() const;

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

    // Loads a legacy sequential palette file (768 or 1024 bytes; the latter
    // carries the alpha ramp).  Throws std::runtime_error when the file cannot
    // be read or has another size.
    [[nodiscard]] static Palette load(const std::filesystem::path& path);

    // Slot-for-slot equality (the reserved slots and the alpha ramp included).
    friend bool operator==(const Palette&, const Palette&) = default;

private:
    std::array<Rgb, slotCount> slots_{};
    std::array<std::uint8_t, slotCount> alpha_{};
    bool hasAlphaRamp_ = false;
    double alphaScale_ = 100.0;   // the byte value that means fully opaque
};

// Count is a sentinel rather than a palette. It exists so a consumer that
// enumerates the builtins -- the Qt layer's builtinPalettes array, which drives
// the menu, the selector and settings restore -- can static_assert that it
// covers all of them. Without it a new enumerator simply never reaches the UI,
// and the palette tests still pass because they pin the names, not the count.
// Add new palettes above it, and handle it in switches by falling through to
// the default palette.
enum class BuiltinPalette {
    Rainbow, Turbo, Viridis, Plasma, Parula, Coolwarm, Blackbody, Count };

// Compiled-in copies of the palette files shipped under palettes/; Rainbow
// is byte-identical to the legacy default `Palette` file and is the default
// for scalar rendering. The rest are curated from the popular visualization
// packages: turbo, viridis, plasma (matplotlib), parula (MATLAB), coolwarm
// (the Moreland diverging map shared by ParaView/VisIt), and blackbody (a
// black-body radiation thermal ramp).
[[nodiscard]] const Palette& builtinPalette(BuiltinPalette palette);
// The palette's settings key, without a file extension, e.g. "rainbow". Also
// the basis for its UI label, which the Qt layer capitalizes -- so this string
// is what goes on disk and must not change with presentation.
[[nodiscard]] std::string_view builtinPaletteName(BuiltinPalette palette) noexcept;

} // namespace amrvis
