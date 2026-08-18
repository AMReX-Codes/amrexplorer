#include <amrexplorer/render2d/Palette.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace amrvis {

Palette::Palette(const std::array<Rgb, slotCount>& slots,
    std::optional<std::array<std::uint8_t, slotCount>> alpha)
    : slots_(slots)
    , alpha_(alpha.value_or(std::array<std::uint8_t, slotCount>{}))
    , hasAlphaRamp_(alpha.has_value())
{
}

bool Palette::hasAlphaRamp() const noexcept
{
    return hasAlphaRamp_;
}

double Palette::opacity(int index) const noexcept
{
    const auto clamped = std::clamp(index, 0, slotCount - 1);
    if (!hasAlphaRamp_) {
        // Amrvis's AV_PAL_NON_ALPHA transfer function.
        return static_cast<double>(clamped) / static_cast<double>(slotCount - 1);
    }
    // Amrvis's AV_PAL_ALPHA transfer function: the byte is a percentage.
    return std::min(1.0,
        static_cast<double>(alpha_[static_cast<std::size_t>(clamped)]) / 100.0);
}

const Palette::Rgb& Palette::slot(int index) const noexcept
{
    const auto clamped = std::clamp(index, 0, slotCount - 1);
    return slots_[static_cast<std::size_t>(clamped)];
}

std::uint32_t Palette::slotArgb(int index) const noexcept
{
    const auto& color = slot(index);
    return 0xFF000000U
        | (static_cast<std::uint32_t>(color.red) << 16U)
        | (static_cast<std::uint32_t>(color.green) << 8U)
        | static_cast<std::uint32_t>(color.blue);
}

Palette Palette::reversed() const
{
    Palette result = *this;
    for (int index = paletteStart; index <= paletteEnd; ++index) {
        const auto mirror = paletteStart + paletteEnd - index;
        result.slots_[static_cast<std::size_t>(index)] =
            slots_[static_cast<std::size_t>(mirror)];
    }
    return result;
}

std::uint32_t Palette::argb(double t) const noexcept
{
    // Legacy clipping: below the range maps to the first data slot, above
    // the range to the last one; NaN fails both tests and maps to the first
    // data slot.
    if (!(t > 0.0)) {
        return slotArgb(paletteStart);
    }
    if (!(t < 1.0)) {
        return slotArgb(paletteEnd);
    }
    // Legacy truncates the scaled value into a slot instead of rounding or
    // interpolating.
    const auto offset = static_cast<int>(t * static_cast<double>(colorSlots - 1));
    return slotArgb(paletteStart + offset);
}

std::uint32_t Palette::levelColor(int level, int maxLevel) const noexcept
{
    // Legacy draws the coarsest drawn level with the plain white pixel.
    if (level <= 0 || maxLevel <= 0) {
        return 0xFFFFFFFFU;
    }
    // Palette::SafePaletteIndex(level, maxLevel) from the legacy code.
    const auto scaled = static_cast<float>(colorSlots - 10)
        * (static_cast<float>(maxLevel - level) / static_cast<float>(maxLevel));
    const auto index = paletteStart + (colorSlots - 1 - static_cast<int>(scaled));
    return slotArgb(std::clamp(index, paletteStart, paletteEnd));
}

Palette Palette::load(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open palette file: " + path.string());
    }
    // Bound the read so an accidentally-selected huge file cannot block the GUI
    // thread or exhaust memory. Read at most one byte more than the largest
    // valid size: gcount is 768 or 1024 only for an exactly-sized file, while
    // anything larger yields 1025 (the cap) and is rejected.
    constexpr std::size_t channelBytes = slotCount;
    constexpr std::size_t maxBytes = 4 * channelBytes + 1;
    std::array<char, maxBytes> buffer{};
    stream.read(buffer.data(), static_cast<std::streamsize>(maxBytes));
    const auto byteCount = static_cast<std::size_t>(stream.gcount());
    if (byteCount != 3 * channelBytes && byteCount != 4 * channelBytes) {
        throw std::runtime_error("palette file " + path.string()
            + " is not a legacy sequential palette (expected 768 or 1024 bytes, got "
            + (byteCount == maxBytes ? std::string("more than 1024")
                                     : std::to_string(byteCount))
            + ")");
    }
    std::array<Rgb, slotCount> slots{};
    for (std::size_t index = 0; index < channelBytes; ++index) {
        slots[index].red = static_cast<std::uint8_t>(buffer[index]);
        slots[index].green = static_cast<std::uint8_t>(buffer[channelBytes + index]);
        slots[index].blue = static_cast<std::uint8_t>(buffer[2 * channelBytes + index]);
    }
    if (byteCount == 3 * channelBytes) {
        return Palette(slots);
    }
    std::array<std::uint8_t, slotCount> alpha{};
    for (std::size_t index = 0; index < channelBytes; ++index) {
        alpha[index] = static_cast<std::uint8_t>(buffer[3 * channelBytes + index]);
    }
    return Palette(slots, alpha);
}

} // namespace amrvis
