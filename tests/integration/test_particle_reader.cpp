#include <amrexplorer/io/ParticleReader.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>

#include <algorithm>
#include <array>
#include <bit>
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

std::uint64_t idcpu(std::int32_t id, std::int32_t cpu)
{
    return (std::uint64_t{1} << 63U)
        | (static_cast<std::uint64_t>(id) << 24U)
        | static_cast<std::uint32_t>(cpu);
}

void writeFixture(const std::filesystem::path& root,
    const std::vector<std::pair<std::int32_t, std::int32_t>>& identities,
    double xOffset, bool expanded = false,
    int additionalRealComponents = 0, bool checkpoint = true)
{
    const auto species = root / "Tracer";
    std::filesystem::create_directories(species / "Level_0");
    {
        std::ofstream header(species / "Header");
        header << (expanded
                ? "Version_Two_Dot_One_double\n"
                : "Version_Two_Dot_Zero_double\n")
               << "2\n"
               << 1 + additionalRealComponents << "\nmass\n";
        for (int component = 0; component < additionalRealComponents;
             ++component) {
            header << "attribute_" << component << '\n';
        }
        header
               << "0\n"
               << (checkpoint ? "1\n" : "0\n")
               << identities.size() << '\n'
               << "1000\n"
               << "0\n"
               << "1\n"
               << "0 " << identities.size() << " 0\n";
    }
    std::ofstream data(species / "Level_0" / "DATA_00000",
        std::ios::binary);
    if (checkpoint) {
        for (const auto [id, cpu] : identities) {
            const auto packed = idcpu(id, cpu);
            const std::array<std::int32_t, 2> record = expanded
                ? std::array{
                    std::bit_cast<std::int32_t>(
                        static_cast<std::uint32_t>(packed >> 32U)),
                    std::bit_cast<std::int32_t>(
                        static_cast<std::uint32_t>(packed))}
                : std::array{id, cpu};
            data.write(
                reinterpret_cast<const char*>(record.data()), sizeof(record));
        }
    }
    for (const auto [id, cpu] : identities) {
        static_cast<void>(cpu);
        std::vector<double> record(
            static_cast<std::size_t>(3 + additionalRealComponents), 0.0);
        record[0] = xOffset + static_cast<double>(id);
        record[1] = 2.0 * static_cast<double>(id);
        record[2] = 0.25 * static_cast<double>(id);
        data.write(reinterpret_cast<const char*>(record.data()),
            static_cast<std::streamsize>(record.size() * sizeof(double)));
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        require(argc == 2, "plotfile fixture path is required");
        const auto root = std::filesystem::temp_directory_path()
            / "amrvis2_particle_reader_test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::vector<std::int32_t> ids{
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::vector<std::pair<std::int32_t, std::int32_t>> identities;
        for (const auto id : ids) {
            identities.emplace_back(id, 7);
        }
        writeFixture(root, identities, 0.0);

        const auto species = amrvis::discoverParticleSpecies(root);
        require(species.size() == 1, "particle species was not discovered");
        require(species.front().name == "Tracer", "wrong particle species name");
        require(species.front().particleCount == ids.size(),
            "wrong particle count");

        const auto all = amrvis::readParticleSample(root, "Tracer", 1.0);
        require(all.points.size() == ids.size(), "full sample omitted particles");
        require(all.points[2].id == idcpu(3, 7),
            "complete particle idcpu was not preserved");
        require(all.points[2].position[0] == 3.0
                && all.points[2].position[1] == 6.0,
            "particle position was decoded incorrectly");

        writeFixture(root, identities, 0.0, false, 0, false);
        const auto nonCheckpointSpecies
            = amrvis::discoverParticleSpecies(root);
        require(nonCheckpointSpecies.size() == 1,
            "non-checkpoint species metadata was not discovered");
        bool rejectedNonCheckpoint = false;
        try {
            static_cast<void>(
                amrvis::readParticleSample(root, "Tracer", 1.0));
        } catch (const amrvis::ParticleReadError& error) {
            rejectedNonCheckpoint = std::string(error.what()).find(
                "non-checkpoint particle data is unsupported")
                != std::string::npos;
        }
        require(rejectedNonCheckpoint,
            "non-checkpoint particle data was not rejected explicitly");

        writeFixture(root, identities, 0.0, true);
        const auto expanded = amrvis::readParticleSample(root, "Tracer", 1.0);
        require(expanded.points.size() == all.points.size()
                && expanded.points[2].id == idcpu(3, 7),
            "expanded particle idcpu was not preserved");

        // Exercise both integer and real record boundaries in the reader's
        // bounded I/O chunks.
        std::vector<std::pair<std::int32_t, std::int32_t>> chunkedIdentities;
        chunkedIdentities.reserve(140'000);
        for (std::int32_t id = 1; id <= 140'000; ++id) {
            chunkedIdentities.emplace_back(id, 7);
        }
        writeFixture(root, chunkedIdentities, 0.0);
        const auto chunked = amrvis::readParticleSample(root, "Tracer", 1.0);
        require(chunked.points.size() == chunkedIdentities.size(),
            "chunked particle read omitted particles");
        require(chunked.points[43'690].position[0] == 43'691.0
                && chunked.points[131'072].id == idcpu(131'073, 7),
            "particle data was decoded incorrectly across chunk boundaries");

        std::vector<std::pair<std::int32_t, std::int32_t>> sparseIdentities;
        sparseIdentities.reserve(20'000);
        for (std::int32_t id = 1; id <= 20'000; ++id) {
            sparseIdentities.emplace_back(id, 7);
        }
        constexpr int extraRealComponents = 128;
        writeFixture(
            root, sparseIdentities, 0.0, false, extraRealComponents);
        const auto sparse
            = amrvis::readParticleSample(root, "Tracer", 0.0002);
        require(sparse.points.size() == 1,
            "sparse fixture did not select the expected particle");
        const auto fullRealPayload = static_cast<std::uint64_t>(
            sparseIdentities.size())
            * static_cast<std::uint64_t>(3 + extraRealComponents)
            * sizeof(double);
        require(sparse.io.realBytesRead < fullRealPayload / 2,
            "sparse sampling read most of the real particle payload");

        writeFixture(root, identities, 0.0);
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
        auto reorderedIdentities = identities;
        std::ranges::reverse(reorderedIdentities);
        writeFixture(root, reorderedIdentities, 100.0);
        const auto nextFrame = amrvis::readParticleSample(root, "Tracer", 0.5);
        std::vector<std::uint64_t> nextIds;
        for (const auto& point : nextFrame.points) {
            nextIds.push_back(point.id);
        }
        std::ranges::sort(nextIds);
        require(selectedIds == nextIds,
            "idcpu sample changed with file order or position");
        require(nextFrame.points.front().position[0] > 100.0,
            "next frame positions were not read");

        // Local particle IDs are reused by MPI ranks. A sampler that drops the
        // persistent CPU bits will always keep or reject this pair together.
        writeFixture(root, {{1, 7}, {1, 8}}, 0.0);
        bool separatedRanks = false;
        for (std::uint64_t seed = 0; seed < 1024; ++seed) {
            const auto sample = amrvis::readParticleSample(
                root, "Tracer", 0.5, seed);
            if (sample.points.size() == 1) {
                separatedRanks = true;
                break;
            }
        }
        require(separatedRanks,
            "sampling collapsed distinct CPUs with the same local particle ID");

        const auto preparedRoot = root / "prepared_plotfile";
        std::filesystem::copy(argv[1], preparedRoot,
            std::filesystem::copy_options::recursive);
        writeFixture(preparedRoot, identities, 0.0);
        auto preparedMetadata
            = amrvis::PlotfileMetadataReader{}.read(preparedRoot);
        amrvis::PlotfileDataset preparedDataset(
            preparedRoot, amrvis::DatasetId{1}, 1024 * 1024,
            std::move(preparedMetadata));
        require(preparedDataset.particleSpecies().size() == 1
                && preparedDataset.particleSpecies().front().name == "Tracer",
            "prepared-metadata dataset did not discover particle species");

        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
