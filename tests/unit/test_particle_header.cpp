// Negative tests for the particle Header parser. A crafted Header is a few
// hundred bytes of text that nonetheless declares how much memory the reader
// should set aside, so every declared count must be bounded before it reaches
// an allocation. Each case asserts the specific ParticleReadError and a message
// substring: a std::bad_alloc here would mean the bound was never applied.
#include <amrexplorer/io/ParticleReader.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

// Everything the parser consumes before the per-level grid counts: version,
// dimension, one real component, no integer components, checkpoint flag,
// particle count, next id, and finest level.
std::string headerThroughFinestLevel(
    std::uint64_t particleCount, int finestLevel)
{
    return "Version_Two_Dot_Zero_double\n"
           "2\n"
           "1\nmass\n"
           "0\n"
           "1\n"
        + std::to_string(particleCount) + "\n"
        + "1000\n"
        + std::to_string(finestLevel) + "\n";
}

void writeSpeciesHeader(
    const std::filesystem::path& plotfile, const std::string& body)
{
    const auto species = plotfile / "Tracer";
    std::filesystem::create_directories(species / "Level_0");
    std::ofstream header(species / "Header", std::ios::trunc);
    header << body;
}

void expectRejected(const std::filesystem::path& plotfile,
    const std::string& body, const char* what, const char* expectedMessage)
{
    writeSpeciesHeader(plotfile, body);
    bool threw = false;
    try {
        (void)amrvis::readParticleSample(plotfile, "Tracer", 1.0);
    } catch (const amrvis::ParticleReadError& error) {
        threw = true;
        if (std::string(error.what()).find(expectedMessage)
            == std::string::npos) {
            std::cerr << "FAILED: " << what
                      << " was rejected for the wrong reason: " << error.what()
                      << '\n';
            ++g_failures;
            return;
        }
    } catch (const std::exception& other) {
        std::cerr << "FAILED: " << what
                  << " threw the wrong exception: " << other.what() << '\n';
        ++g_failures;
        return;
    }
    require(threw, what);
}

} // namespace

int main()
{
    const auto scratch = std::filesystem::temp_directory_path()
        / "amrexplorer_particle_header_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    // Baseline: the shape the crafted cases are built from must itself parse,
    // or a rejection below proves nothing about the check it claims to cover.
    {
        const auto plotfile = scratch / "valid";
        writeSpeciesHeader(plotfile,
            headerThroughFinestLevel(0, 0) + "1\n" + "0 0 0\n");
        try {
            const auto sample
                = amrvis::readParticleSample(plotfile, "Tracer", 1.0);
            require(sample.species.dimension == 2,
                "baseline particle Header dimension mismatch");
            require(sample.points.empty(),
                "baseline particle Header should hold no points");
        } catch (const std::exception& error) {
            std::cerr << "FAILED: baseline particle Header was rejected: "
                      << error.what() << '\n';
            ++g_failures;
        }
    }

    // One level over the per-level cap. This bound already existed; it is here
    // so the total-cap case below is known to be testing the *sum* rather than
    // re-testing this.
    expectRejected(scratch / "per_level",
        headerThroughFinestLevel(0, 0) + "10000001\n",
        "a level declaring more grids than the per-level cap",
        "particle grid count is outside supported bounds");

    // Ten levels, each well under the per-level cap, summing to twenty million
    // grids. Before the total was bounded this reserved the sum -- roughly half
    // a gigabyte of GridRecords -- from these few dozen bytes of text.
    {
        std::string body = headerThroughFinestLevel(0, 9);
        for (int level = 0; level < 10; ++level) {
            body += "2000000\n";
        }
        expectRejected(scratch / "cross_level",
            body, "levels summing past the whole-table cap",
            "particle grid total is outside supported bounds");
    }

    // An over-long component name, with no whitespace before the field that
    // follows it. Bounding the extraction with width() is not enough on its
    // own: >> stops at the limit but only fails on an empty extraction, so the
    // tail would be read as the *next* field and every field after it would
    // shift -- a silent misparse, which is worse than the allocation it
    // replaced. It has to be rejected outright.
    {
        // No whitespace between the token and the fields after it, and exactly
        // the ceiling's worth of it: width() stops at 4096 and the residue then
        // parses cleanly as intComponentCount, checkpoint, particleCount,
        // nextId, finestLevel, one grid count and one grid record. Without the
        // rejection this Header is *accepted* and returns an empty sample --
        // no throw at all, which is the failure mode the fix exists for. A
        // token followed by a newline would only ever produce a different
        // error message, which is not the same property.
        std::string body = "Version_Two_Dot_Zero_double\n2\n1\n";
        body += std::string(4096, 'A');
        body += "0 1 0 1000 0 1 0 0 0\n";
        expectRejected(scratch / "long_token", body,
            "a component name past the token ceiling",
            "exceeds the supported length");
    }

    // A forged particleCount over an empty DATA file. The Header is internally
    // consistent -- its one grid record accounts for every claimed particle --
    // so nothing in the text contradicts it, and the reserve was previously
    // taken from it directly: a trillion points is 32 TB of ParticlePoint.
    // The files on disk are the only evidence of how many particles exist, and
    // reading now fails on the truncated grid rather than on the allocation.
    {
        const auto plotfile = scratch / "forged_count";
        writeSpeciesHeader(plotfile,
            headerThroughFinestLevel(1'000'000'000'000ULL, 0) + "1\n"
                + "0 1000000000000 0\n");
        std::ofstream(plotfile / "Tracer" / "Level_0" / "DATA_00000",
            std::ios::binary | std::ios::trunc);
        bool threw = false;
        try {
            (void)amrvis::readParticleSample(plotfile, "Tracer", 1.0);
        } catch (const amrvis::ParticleReadError& error) {
            threw = true;
            require(std::string(error.what()).find("truncated particle grid")
                    != std::string::npos,
                "a forged particle count was rejected for the wrong reason");
        } catch (const std::exception& other) {
            std::cerr << "FAILED: a forged particle count threw the wrong "
                         "exception: "
                      << other.what() << '\n';
            ++g_failures;
        }
        require(threw, "a forged particle count over an empty DATA file");
    }

    // The other side of the reserve bound: a species whose DATA file really
    // does hold what its Header claims comes back whole. Every case above is
    // a rejection, so without this one the file-size pass and the ceiling
    // arithmetic are only ever exercised on inputs that end in a throw.
    //
    // Note what this does and does not pin down. The ceiling feeds a reserve,
    // which is capacity and not a limit, so an off-by-one there cannot change
    // the result -- only the allocation. What this catches is the pass
    // throwing, dividing by zero, or otherwise disturbing a valid read.
    {
        const auto plotfile = scratch / "valid_points";
        constexpr int count = 4;
        writeSpeciesHeader(plotfile,
            headerThroughFinestLevel(count, 0) + "1\n"
                + "0 " + std::to_string(count) + " 0\n");
        // Two int32 identity words then dimension + 1 doubles, per particle,
        // matching the 2-D single-real-component Header above.
        std::ofstream data(plotfile / "Tracer" / "Level_0" / "DATA_00000",
            std::ios::binary | std::ios::trunc);
        for (int i = 0; i < count; ++i) {
            const std::array<std::int32_t, 2> identity{i + 1, 0};
            data.write(reinterpret_cast<const char*>(identity.data()),
                sizeof(identity));
        }
        for (int i = 0; i < count; ++i) {
            const std::array<double, 3> record{
                0.5 * i, 0.25 * i, static_cast<double>(i)};
            data.write(reinterpret_cast<const char*>(record.data()),
                sizeof(record));
        }
        data.close();
        try {
            const auto sample
                = amrvis::readParticleSample(plotfile, "Tracer", 1.0);
            require(sample.points.size() == count,
                "the reserve bound truncated a sample its files can hold");
        } catch (const std::exception& error) {
            std::cerr << "FAILED: a valid species was rejected: "
                      << error.what() << '\n';
            ++g_failures;
        }
    }

    std::filesystem::remove_all(scratch);
    if (g_failures != 0) {
        std::cerr << g_failures << " particle Header check(s) failed\n";
        return 1;
    }
    return 0;
}
