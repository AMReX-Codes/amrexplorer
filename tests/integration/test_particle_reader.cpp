#include <amrexplorer/io/ParticleReader.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeFixture(const std::filesystem::path& root,
    const std::vector<std::int32_t>& ids, double xOffset, std::int32_t cpu)
{
    const auto species = root / "Tracer";
    std::filesystem::create_directories(species / "Level_0");
    {
        std::ofstream header(species / "Header");
        header << "Version_Two_Dot_Zero_double\n"
               << "2\n"
               << "1\nmass\n"
               << "0\n"
               << "1\n"
               << ids.size() << '\n'
               << "1000\n"
               << "0\n"
               << "1\n"
               << "0 " << ids.size() << " 0\n";
    }
    std::ofstream data(species / "Level_0" / "DATA_00000",
        std::ios::binary);
    for (const auto id : ids) {
        const std::array<std::int32_t, 2> record{id, cpu};
        data.write(reinterpret_cast<const char*>(record.data()), sizeof(record));
    }
    for (const auto id : ids) {
        const std::array<double, 3> record{
            xOffset + static_cast<double>(id),
            2.0 * static_cast<double>(id),
            0.25 * static_cast<double>(id)};
        data.write(reinterpret_cast<const char*>(record.data()), sizeof(record));
    }
}

} // namespace

int main()
{
    try {
        const auto root = std::filesystem::temp_directory_path()
            / "amrvis2_particle_reader_test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::vector<std::int32_t> ids{
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        writeFixture(root, ids, 0.0, 7);

        const auto species = amrvis::discoverParticleSpecies(root);
        require(species.size() == 1, "particle species was not discovered");
        require(species.front().name == "Tracer", "wrong particle species name");
        require(species.front().particleCount == ids.size(),
            "wrong particle count");

        const auto all = amrvis::readParticleSample(root, "Tracer", 1.0);
        require(all.points.size() == ids.size(), "full sample omitted particles");
        require(all.points[2].id == 3, "particle ID was not preserved");
        require(all.points[2].position[0] == 3.0
                && all.points[2].position[1] == 6.0,
            "particle position was decoded incorrectly");

        const auto half = amrvis::readParticleSample(root, "Tracer", 0.5);
        const auto quarter = amrvis::readParticleSample(root, "Tracer", 0.25);
        require(!half.points.empty() && half.points.size() < all.points.size(),
            "half sample is not a subset");
        for (const auto& point : quarter.points) {
            require(std::ranges::any_of(half.points,
                [&point](const auto& candidate) {
                    return candidate.id == point.id;
                }), "lower fraction is not nested in higher fraction");
        }

        auto selectedIds = [&half] {
            std::vector<std::uint64_t> result;
            for (const auto& point : half.points) {
                result.push_back(point.id);
            }
            return result;
        }();
        std::ranges::sort(selectedIds);
        auto reorderedIds = ids;
        std::ranges::reverse(reorderedIds);
        writeFixture(root, reorderedIds, 100.0, 99);
        const auto nextFrame = amrvis::readParticleSample(root, "Tracer", 0.5);
        std::vector<std::uint64_t> nextIds;
        for (const auto& point : nextFrame.points) {
            nextIds.push_back(point.id);
        }
        std::ranges::sort(nextIds);
        require(selectedIds == nextIds,
            "ID sample changed with file order, CPU rank, or position");
        require(nextFrame.points.front().position[0] > 100.0,
            "next frame positions were not read");

        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
