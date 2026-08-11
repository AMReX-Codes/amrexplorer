// Negative tests for the particle Header parser. A crafted Header is a few
// hundred bytes of text that nonetheless declares how much memory the reader
// should set aside, so every declared count must be bounded before it reaches
// an allocation. Each case asserts the specific ParticleReadError and a message
// substring: a std::bad_alloc here would mean the bound was never applied.
#include <amrexplorer/io/ParticleReader.hpp>

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

    std::filesystem::remove_all(scratch);
    if (g_failures != 0) {
        std::cerr << g_failures << " particle Header check(s) failed\n";
        return 1;
    }
    return 0;
}
