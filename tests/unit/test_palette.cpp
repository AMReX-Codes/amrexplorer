#include <amrexplorer/render2d/Palette.hpp>
#include <amrexplorer/render2d/ScalarRenderer.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::vector<char> readBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "usage: test_palette <path to palettes/rainbow.pal>");
    const std::filesystem::path rainbowPath(argv[1]);

    const auto fileBytes = readBytes(rainbowPath);
    // The 1024-byte legacy file: the alpha checks below index its ramp.
    require(fileBytes.size() == 1024,
        "rainbow.pal is not the 1024-byte legacy sequential palette");

    // The builtin rainbow reproduces the legacy palette file bytes exactly.
    const auto& rainbow = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    bool channelsMatch = true;
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        const auto offset = static_cast<std::size_t>(index);
        const auto& color = rainbow.slot(index);
        channelsMatch = channelsMatch
            && color.red == static_cast<std::uint8_t>(fileBytes[offset])
            && color.green == static_cast<std::uint8_t>(fileBytes[256 + offset])
            && color.blue == static_cast<std::uint8_t>(fileBytes[512 + offset]);
    }
    require(channelsMatch, "builtin rainbow does not match rainbow.pal bytes");

    // load() round-trips the same byte content.
    const auto loaded = amrvis::Palette::load(rainbowPath);
    bool roundTrip = true;
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        roundTrip = roundTrip && loaded.slotArgb(index) == rainbow.slotArgb(index);
    }
    require(roundTrip, "Palette::load did not round-trip the builtin rainbow");

    // load() rejects a truncated file.
    const auto truncatedPath = std::filesystem::temp_directory_path()
        / "amrexplorer_test_palette_truncated.pal";
    {
        std::ofstream stream(truncatedPath, std::ios::binary);
        stream.write(fileBytes.data(), 100);
    }
    bool threw = false;
    try {
        (void)amrvis::Palette::load(truncatedPath);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    std::filesystem::remove(truncatedPath);
    require(threw, "Palette::load accepted a truncated palette file");

    // load() rejects an unreadable file.
    threw = false;
    try {
        (void)amrvis::Palette::load(rainbowPath.parent_path() / "no_such_palette.pal");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "Palette::load accepted a missing palette file");

    // load() accepts a 768-byte (3-channel) palette: the rainbow bytes minus
    // the unused fourth channel round-trip to the same colors.
    const auto palette768Path = std::filesystem::temp_directory_path()
        / "amrexplorer_test_palette_768.pal";
    {
        std::ofstream stream(palette768Path, std::ios::binary);
        stream.write(fileBytes.data(), 3 * amrvis::Palette::slotCount);
    }
    const auto loaded768 = amrvis::Palette::load(palette768Path);
    bool roundTrip768 = true;
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        roundTrip768 = roundTrip768
            && loaded768.slotArgb(index) == rainbow.slotArgb(index);
    }
    require(roundTrip768, "Palette::load did not round-trip a 768-byte palette");
    std::filesystem::remove(palette768Path);

    // The fourth channel is the legacy alpha ramp, kept for volume rendering
    // with Amrvis's semantics: a percentage per slot (rainbow.pal carries the
    // legacy 0..100 ramp), clamped to 1. The builtin rainbow carries the same
    // ramp, so it equals the file outright. A 768-byte palette has none and
    // reads back Amrvis's default transfer function, the linear ramp.
    // Reversal leaves the ramp alone -- opacity follows the value, not the
    // color -- and equality sees it.
    require(loaded.hasAlphaRamp() && !loaded768.hasAlphaRamp()
            && rainbow.hasAlphaRamp() && rainbow == loaded,
        "the alpha ramp's presence was not read from the file size, or the "
        "builtin rainbow lacks the file's ramp");
    require(loaded.opacity(0) == 0.0 && loaded.opacity(255) == 1.0,
        "the legacy alpha ramp does not span transparent to opaque");
    // The legacy ramp is hand-made (one dip near slot 92), so pin bounds and
    // the percentage reading rather than monotonicity: byte 128 of the file's
    // ramp is the opacity of slot 128.
    bool bounded = true;
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        bounded = bounded && loaded.opacity(index) >= 0.0
            && loaded.opacity(index) <= 1.0;
    }
    require(bounded, "the legacy alpha ramp left [0, 1]");
    require(loaded.opacity(128)
            == static_cast<double>(static_cast<std::uint8_t>(fileBytes[768 + 128]))
                / 100.0,
        "the alpha byte was not read as a percentage");
    require(loaded.opacity(-1) == loaded.opacity(0)
            && loaded.opacity(1000) == loaded.opacity(255),
        "opacity did not clamp the slot index");
    require(loaded768.opacity(0) == 0.0 && loaded768.opacity(255) == 1.0
            && loaded768.opacity(51) == 51.0 / 255.0
            && amrvis::Palette(std::array<amrvis::Palette::Rgb,
                    amrvis::Palette::slotCount>{}).opacity(51) == 51.0 / 255.0,
        "a palette without a ramp did not use the linear default");
    // Every builtin equals its shipped .pal file, colours and ramp: the
    // generated ones carry the legacy default 0..100 ramp.
    for (int index = 0; index < static_cast<int>(amrvis::BuiltinPalette::Count); ++index) {
        const auto palette = static_cast<amrvis::BuiltinPalette>(index);
        const auto& builtin = amrvis::builtinPalette(palette);
        const std::string name(amrvis::builtinPaletteName(palette));
        const auto file = rainbowPath.parent_path() / (name + ".pal");
        require(std::filesystem::exists(file),
            ("the shipped .pal file is missing for " + name).c_str());
        require(builtin == amrvis::Palette::load(file),
            ("the builtin does not equal its shipped .pal file: " + name).c_str());
        require(builtin.hasAlphaRamp() && builtin.opacity(0) == 0.0
                && builtin.opacity(255) == 1.0,
            ("the alpha ramp does not span transparent to opaque: " + name).c_str());
    }
    {
        auto slots = std::array<amrvis::Palette::Rgb, amrvis::Palette::slotCount>{};
        auto alpha = std::array<std::uint8_t, amrvis::Palette::slotCount>{};
        // A ramp with a byte above 100 cannot be a legacy percent ramp: it is
        // read full-scale, so a 0..255 linear ramp reads as one, and the flat
        // 255 older builds wrote reads as opaque.
        for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
            alpha[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(index);
        }
        const amrvis::Palette fullScale(slots, alpha);
        require(fullScale.hasAlphaRamp() && fullScale.opacity(0) == 0.0
                && fullScale.opacity(255) == 1.0
                && fullScale.opacity(51) == 51.0 / 255.0,
            "a 0..255 ramp was not read full-scale");
        alpha.fill(255);
        const amrvis::Palette flat(slots, alpha);
        require(flat.opacity(7) == 1.0 && flat.opacity(255) == 1.0,
            "a flat 255 ramp did not read as fully opaque");
        // Equality sees the ramp's contents: the same colours with one alpha
        // byte changed differ, and the same alpha with one colour changed
        // differ; identical inputs are equal.
        auto alphaVariant = alpha;
        alphaVariant[100] = 7;
        auto slotsVariant = slots;
        slotsVariant[100].green ^= 1U;
        require(amrvis::Palette(slots, alpha) == flat
                && !(amrvis::Palette(slots, alphaVariant) == flat)
                && !(amrvis::Palette(slotsVariant, alpha) == flat)
                && !(flat == amrvis::Palette(slots)),
            "Palette equality does not follow the slots and the alpha ramp");
        // A reserved slot's byte does not decide the scale: a percent ramp
        // with an outlier at slot 0 still reads as percent -- and that
        // outlier, 255 % of opaque, clamps to 1.
        auto percent = std::array<std::uint8_t, amrvis::Palette::slotCount>{};
        for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
            percent[static_cast<std::size_t>(index)]
                = static_cast<std::uint8_t>((100 * index) / 255);
        }
        percent[0] = 255;
        require(amrvis::Palette(slots, percent).opacity(255) == 1.0
                && amrvis::Palette(slots, percent).opacity(128)
                    == static_cast<double>((100 * 128) / 255) / 100.0
                && amrvis::Palette(slots, percent).opacity(0) == 1.0,
            "a reserved-slot byte changed the ramp's scale, or was not clamped");
        // The sniff's edges: byte 100 in the last data slot is still percent
        // (slot 128 reads 50 %), byte 101 there is full-scale (slot 128
        // reads 50 / 255) -- so the last data slot is inspected, and the
        // threshold sits exactly at 100.
        auto edge = percent;
        edge[0] = 0;
        edge[128] = 50;
        edge[255] = 100;
        require(amrvis::Palette(slots, edge).opacity(128) == 0.5,
            "a data byte of 100 did not read as percent");
        edge[255] = 101;
        require(amrvis::Palette(slots, edge).opacity(128) == 50.0 / 255.0,
            "a data byte of 101 in the last slot did not switch to full-scale");
        // A plane whose data slots are all zero is no ramp: the linear
        // default applies, whatever the reserved slots hold.
        auto zero = std::array<std::uint8_t, amrvis::Palette::slotCount>{};
        zero[1] = 100;
        const amrvis::Palette zeroed(slots, zero);
        require(!zeroed.hasAlphaRamp() && zeroed.opacity(255) == 1.0
                && zeroed == amrvis::Palette(slots),
            "an all-zero data plane was taken as an authored ramp");
    }
    require(loaded.reversed().opacity(3) == loaded.opacity(3)
            && loaded.reversed().opacity(255) == loaded.opacity(255)
            && loaded.reversed().hasAlphaRamp(),
        "reversed() disturbed the alpha ramp");

    // load() rejects a file larger than 1024 bytes (the bounded read caps it).
    const auto oversizedPath = std::filesystem::temp_directory_path()
        / "amrexplorer_test_palette_oversized.pal";
    {
        const std::vector<char> bytesOver(static_cast<std::size_t>(2000), 0);
        std::ofstream stream(oversizedPath, std::ios::binary);
        stream.write(bytesOver.data(),
            static_cast<std::streamsize>(bytesOver.size()));
    }
    threw = false;
    std::string overMessage;
    try {
        (void)amrvis::Palette::load(oversizedPath);
    } catch (const std::runtime_error& error) {
        threw = true;
        overMessage = error.what();
    }
    std::filesystem::remove(oversizedPath);
    require(threw, "Palette::load accepted an oversized palette file");
    require(overMessage.find("more than 1024") != std::string::npos,
        "oversized palette rejection did not report the size");

    // The normalized endpoints map to distinct colors and out-of-range t clamps.
    require(rainbow.argb(0.0) != rainbow.argb(1.0),
        "normalized endpoints mapped to one color");
    require(rainbow.argb(-0.5) == rainbow.argb(0.0), "argb did not clamp below zero");
    require(rainbow.argb(1.5) == rainbow.argb(1.0), "argb did not clamp above one");

    // Non-finite inputs follow the same legacy clipping. NaN fails both the
    // t > 0 and t < 1 tests and maps to the first data slot (paletteStart) --
    // the documented behavior that had no coverage. -inf lands there too, while
    // +inf maps to the last data slot (paletteEnd).
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    require(rainbow.argb(nan) == rainbow.slotArgb(amrvis::Palette::paletteStart),
        "argb(NaN) did not map to the first data slot");
    require(rainbow.argb(nan) == rainbow.argb(0.0),
        "argb(NaN) disagreed with the clamped-below-zero color");
    require(rainbow.argb(-infinity) == rainbow.slotArgb(amrvis::Palette::paletteStart),
        "argb(-inf) did not map to the first data slot");
    require(rainbow.argb(infinity) == rainbow.slotArgb(amrvis::Palette::paletteEnd),
        "argb(+inf) did not map to the last data slot");

    // The coarsest drawn level is white-ish, finer levels use palette colors.
    const auto coarseColor = rainbow.levelColor(0, 3);
    require((coarseColor & 0x00FFFFFFU) == 0x00FFFFFFU, "level 0 color is not white");
    require(rainbow.levelColor(1, 3) != coarseColor, "finer level reused the white color");

    // The scalar renderer defaults to the builtin rainbow palette.
    amrvis::ScalarPlane plane;
    plane.width = 2;
    plane.height = 1;
    plane.values = {0.0F, 1.0F};
    plane.valid = {1, 1};
    plane.sourceLevel = {0, 0};
    const amrvis::ScalarRenderSettings settings;
    require(settings.palette == nullptr, "default render settings carry a palette");
    const auto image = amrvis::renderScalarPlane(plane, settings);
    require(image.valid(), "renderer produced an invalid image buffer");
    require(image.rgba[0] == rainbow.argb(0.0), "minimum value color mismatch");
    require(image.rgba[1] == rainbow.argb(1.0), "maximum value color mismatch");
    require(image.rgba[0] != image.rgba[1], "renderer endpoints mapped to one color");

    // reversed(): the data color range [paletteStart, paletteEnd] is mirrored
    // while the reserved slots (white/black/body) stay put.
    const auto reversedRainbow = rainbow.reversed();
    bool dataReversed = true;
    for (int index = amrvis::Palette::paletteStart;
         index <= amrvis::Palette::paletteEnd; ++index) {
        const auto mirror = amrvis::Palette::paletteStart
            + amrvis::Palette::paletteEnd - index;
        dataReversed = dataReversed
            && reversedRainbow.slotArgb(index) == rainbow.slotArgb(mirror);
    }
    require(dataReversed, "reversed() did not mirror the data color range");
    require(reversedRainbow.slotArgb(amrvis::Palette::whiteIndex)
                == rainbow.slotArgb(amrvis::Palette::whiteIndex)
            && reversedRainbow.slotArgb(amrvis::Palette::blackIndex)
                == rainbow.slotArgb(amrvis::Palette::blackIndex)
            && reversedRainbow.slotArgb(amrvis::Palette::bodyIndex)
                == rainbow.slotArgb(amrvis::Palette::bodyIndex),
        "reversed() disturbed the reserved slots");

    // The value-to-color mapping is flipped end to end (the "_r" variant).
    require(reversedRainbow.argb(0.0) == rainbow.argb(1.0)
            && reversedRainbow.argb(1.0) == rainbow.argb(0.0),
        "reversed() did not flip the value-to-color endpoints");

    // Reversing twice restores the original palette exactly (an involution).
    const auto restored = reversedRainbow.reversed();
    bool involution = true;
    for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
        involution = involution
            && restored.slotArgb(index) == rainbow.slotArgb(index);
    }
    require(involution, "reversing twice did not restore the palette");

    // operator== is slot-for-slot: the involution above is equality, the
    // reversal is not, and a single differing slot breaks it.
    require(restored == rainbow && !(reversedRainbow == rainbow),
        "Palette equality does not follow the slots");
    {
        auto slots = std::array<amrvis::Palette::Rgb, amrvis::Palette::slotCount>{};
        for (int index = 0; index < amrvis::Palette::slotCount; ++index) {
            slots[static_cast<std::size_t>(index)] = rainbow.slot(index);
        }
        slots[amrvis::Palette::whiteIndex].red ^= 1U;
        require(!(amrvis::Palette(slots) == rainbow),
            "Palette equality ignored a reserved slot");
    }

    // These seven strings are not just menu labels: the Qt layer writes the
    // active one into QSettings under "palette/builtin" and matches it back on
    // startup, so changing one silently resets that user's palette to rainbow
    // on upgrade. Renaming a palette is therefore a settings-format change and
    // needs a migration, not just an edit here.
    const std::array<std::pair<amrvis::BuiltinPalette, std::string_view>, 7>
        persistedNames{{
            {amrvis::BuiltinPalette::Rainbow, "rainbow"},
            {amrvis::BuiltinPalette::Turbo, "turbo"},
            {amrvis::BuiltinPalette::Viridis, "viridis"},
            {amrvis::BuiltinPalette::Plasma, "plasma"},
            {amrvis::BuiltinPalette::Parula, "parula"},
            {amrvis::BuiltinPalette::Coolwarm, "coolwarm"},
            {amrvis::BuiltinPalette::Blackbody, "blackbody"},
        }};
    for (const auto& [palette, expected] : persistedNames) {
        require(amrvis::builtinPaletteName(palette) == expected,
            "a builtin palette name changed; stored palette/builtin settings "
            "will no longer match");
    }
    return 0;
}
