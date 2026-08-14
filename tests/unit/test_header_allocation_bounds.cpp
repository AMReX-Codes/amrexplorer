// The reserve bounds in readVisMfIndex are invisible to an ordinary parse test.
// A crafted BoxArray count is rejected either way -- the boxes it promises are
// not in the file -- and the exception says nothing about how much memory was
// taken to produce it. The defect was never the rejection; it was that a Header
// of a few dozen bytes could make the reader reserve a quarter of a gigabyte
// before parsing a single entry, which in the long-lived server is a cost every
// session pays rather than one process.
//
// So this measures instead: global operator new is replaced to count the bytes
// requested, and the crafted parse must stay far below what its declared count
// would take. Without the file-size evidence bound, ten million boxes is about
// 360 MB of IntBox reserved from 42 bytes of text.
//
// The BoxArray reserve is the one measured because it is the only one reachable
// before any evidence has been parsed. The location table's reserve comes after
// boxCount real boxes have been read out of the file, so by then the file is
// genuinely as large as the count claims; its bound is defensive rather than
// load-bearing.
#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/io/PlotfileMetadataReader.hpp>
#include <amrexplorer/io/detail/VisMfIndex.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <system_error>

namespace {

std::atomic<std::size_t> g_allocatedBytes{0};

int g_failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

} // namespace

// Replaced as a matched pair over malloc/free, so nothing is released by an
// allocator that did not produce it. The aligned overloads are deliberately
// left to the library -- they pair with the library's aligned delete, and
// nothing on this path uses them.
//
// "Nothing on this path uses them" is the assumption the measurement rests on,
// so it is asserted rather than trusted: an over-aligned element type would
// route std::vector's allocation to operator new(size_t, align_val_t), the
// counter would stop seeing the reserve, and every case here would pass with
// the bounds reverted -- failing open, silently.
static_assert(alignof(amrvis::IntBox) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "IntBox is over-aligned, so vector routes around the counted operator new "
    "and these allocation bounds are no longer measured");
static_assert(alignof(double) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "double is over-aligned, so the statistics matrix is no longer measured");
void* operator new(std::size_t bytes)
{
    g_allocatedBytes.fetch_add(bytes, std::memory_order_relaxed);
    // malloc(0) may return null, which would look like exhaustion here.
    void* const memory = std::malloc(bytes == 0 ? 1 : bytes);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t bytes)
{
    return operator new(bytes);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

// The budget must sit above what a crafted parse legitimately needs -- a
// handful of small vectors and a stream buffer, well under 100 KB -- and below
// what the unbounded reserve would take, which differs per case: ten million
// IntBox is ~360 MB, while a hundred thousand FieldMetadata is only ~7 MB. A
// single generous budget would leave the component case passing either way, so
// each caller states its own.
constexpr std::size_t defaultAllowedBytes = 8U * 1024U * 1024U;

template <typename Parse>
void requireBoundedAllocation(Parse parse, const char* what,
    std::size_t allowedBytes = defaultAllowedBytes)
{
    const auto before = g_allocatedBytes.load(std::memory_order_relaxed);
    bool threw = false;
    try {
        parse();
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    } catch (const std::exception& other) {
        std::cerr << "FAILED: " << what << " threw the wrong exception: "
                  << other.what() << '\n';
        ++g_failures;
    }
    const auto used
        = g_allocatedBytes.load(std::memory_order_relaxed) - before;
    require(threw, what);
    if (used >= allowedBytes) {
        std::cerr << "FAILED: " << what << " allocated " << used
                  << " bytes; the reserve is not bounded by the file's size\n";
        ++g_failures;
    }
}

int main()
{
    const auto scratch = std::filesystem::temp_directory_path()
        / "amrexplorer_header_allocation_test";
    std::filesystem::create_directories(scratch);
    const auto path = scratch / "claims_ten_million_H";

    // A well-formed prefix whose BoxArray header claims the per-level maximum
    // (the cap itself accepts exactly ten million) and then supplies one box.
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "1\n1\n2\n0\n"
                  "(10000000 0\n"
                  "((0,0) (1,3) (0,0))\n"
                  ")\n";
    }

    requireBoundedAllocation([&path] { (void)amrvis::detail::readVisMfIndex(path, 2); },
        "a VisMF BoxArray shorter than its declared count");

    // The same claim in the top-level plotfile Header, which is the first file
    // any open reads and the one the server reads on every session. Its
    // per-level grid count carries the same ten-million cap, so a Header that
    // declares it and then supplies no grid bounds asks for the same ~360 MB
    // before failing on the first one it cannot read.
    const auto plotfile = scratch / "claims_ten_million_grids";
    std::filesystem::create_directories(plotfile);
    {
        std::ofstream stream(plotfile / "Header", std::ios::binary);
        stream << "HyperCLaw-V1.1\n"      // file version
                  "1\ndensity\n"          // one component
                  "2\n0.0\n0\n"           // dimension, time, finest level
                  "0.0\n0.0\n1.0\n1.0\n"  // physical bounds
                  "((0,0) (7,7) (0,0))\n" // level 0 domain
                  "0\n"                   // level step
                  "0.125 0.125\n"         // cell sizes
                  "0\n0\n"                // coordinate system, boundary width
                  "0 10000000 0.0 0\n";   // level record claiming ten million
    }
    requireBoundedAllocation(
        [&plotfile] { (void)amrvis::PlotfileMetadataReader{}.read(plotfile); },
        "a plotfile Header level record shorter than its declared grid count");

    // A component count is the same shape of claim, one line per name.
    const auto components = scratch / "claims_many_components";
    std::filesystem::create_directories(components);
    {
        std::ofstream stream(components / "Header", std::ios::binary);
        stream << "HyperCLaw-V1.1\n100000\ndensity\n";
    }
    requireBoundedAllocation(
        [&components] { (void)amrvis::PlotfileMetadataReader{}.read(components); },
        "a plotfile Header shorter than its declared component count",
        1024U * 1024U);

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " header_allocation_bounds test failure(s)\n";
        return 1;
    }
    return 0;
}
