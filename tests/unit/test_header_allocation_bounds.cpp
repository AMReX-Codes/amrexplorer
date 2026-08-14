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
#include <amrexplorer/data/LocalDatasetSession.hpp>
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

std::atomic<std::size_t> g_liveBytes{0};
std::atomic<std::size_t> g_peakBytes{0};

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
// One per element type any case here measures, since an assert that covers
// only some of them leaves the rest free to fail open.
static_assert(alignof(amrvis::IntBox) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "IntBox is over-aligned, so vector routes around the counted operator new "
    "and these allocation bounds are no longer measured");
static_assert(alignof(double) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "double is over-aligned, so the statistics matrix is no longer measured");
static_assert(alignof(amrvis::FieldMetadata) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "FieldMetadata is over-aligned, so the component-count case is no longer "
    "measured");
static_assert(alignof(std::string) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
    "std::string is over-aligned, so the FabOnDisk name reserve is no longer "
    "measured");
// Live bytes and their high-water mark, not a running total. The property
// under test is how much a crafted parse *holds*, and this PR deliberately
// replaces up-front sizing with growth by reallocation past the reserve cap --
// so a cumulative counter would charge every doubling's new buffer while the
// old one is freed moments later, and eventually fail a bound that is working.
// The size is stashed ahead of the block so the unsized deletes can return it.
struct AllocationHeader {
    std::size_t bytes;
};
constexpr std::size_t headerBytes = sizeof(AllocationHeader) < alignof(std::max_align_t)
    ? alignof(std::max_align_t)
    : sizeof(AllocationHeader);

void recordPeak(std::size_t live)
{
    auto previous = g_peakBytes.load(std::memory_order_relaxed);
    while (live > previous
        && !g_peakBytes.compare_exchange_weak(previous, live,
               std::memory_order_relaxed)) {
    }
}

void* operator new(std::size_t bytes)
{
    void* const block = std::malloc(bytes + headerBytes);
    if (block == nullptr) {
        throw std::bad_alloc();
    }
    static_cast<AllocationHeader*>(block)->bytes = bytes;
    const auto live
        = g_liveBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    recordPeak(live);
    return static_cast<char*>(block) + headerBytes;
}

void* operator new[](std::size_t bytes)
{
    return operator new(bytes);
}

void operator delete(void* memory) noexcept
{
    if (memory == nullptr) {
        return;
    }
    auto* const block = static_cast<char*>(memory) - headerBytes;
    g_liveBytes.fetch_sub(reinterpret_cast<AllocationHeader*>(block)->bytes,
        std::memory_order_relaxed);
    std::free(block);
}

void operator delete[](void* memory) noexcept
{
    operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    operator delete(memory);
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
    // The peak is measured against what was already live when the case began,
    // so the number reported is what this parse added at its worst moment.
    const auto before = g_liveBytes.load(std::memory_order_relaxed);
    g_peakBytes.store(before, std::memory_order_relaxed);
    bool threw = false;
    try {
        parse();
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    } catch (const std::exception& other) {
        std::cerr << "FAILED: " << what << " threw the wrong exception: "
                  << other.what() << '\n';
        ++g_failures;
        return;  // one failure per case; require(threw) below would add another
    }
    const auto used = g_peakBytes.load(std::memory_order_relaxed) - before;
    require(threw, what);
    // The positive control. Every one of these parses allocates *something*;
    // a zero means the replaced operator new stopped being the allocator this
    // binary uses -- a shared build, an LTO or allocator change -- at which
    // point every case below passes with the bounds reverted, which is the
    // silent-green failure this file exists to avoid.
    if (used == 0) {
        std::cerr << "FAILED: " << what
                  << " allocated nothing, so the counted operator new is not "
                     "in effect and these bounds are not being measured\n";
        ++g_failures;
        return;
    }
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

    // Forged evidence. std::filesystem::file_size reports the *apparent* size,
    // so extending a short crafted header makes it claim megabytes it does not
    // occupy -- one filesystem block, on any filesystem with sparse support.
    // The file-size bound alone is therefore defeatable at almost no cost,
    // which is what the absolute reserve cap is for. 4 MB of apparent size
    // would otherwise vouch for ~500,000 boxes, or ~18 MB of IntBox.
    const auto sparse = scratch / "sparse_claims_ten_million_H";
    {
        std::ofstream stream(sparse, std::ios::binary);
        stream << "1\n1\n2\n0\n"
                  "(10000000 0\n"
                  "((0,0) (1,3) (0,0))\n"
                  ")\n";
    }
    std::error_code resizeError;
    std::filesystem::resize_file(sparse, 4u * 1024u * 1024u, resizeError);
    require(!resizeError, "could not extend the sparse header fixture");
    requireBoundedAllocation(
        [&sparse] { (void)amrvis::detail::readVisMfIndex(sparse, 2); },
        "a sparse header forging four megabytes of apparent size");

    // The statistics matrix: both factors pass their own checks, and it is
    // their product that allocates. Ten real boxes and a declared 100,000
    // components claim a million doubles from a header with none of them.
    const auto matrix = scratch / "claims_wide_matrix_H";
    {
        std::ofstream stream(matrix, std::ios::binary);
        stream << "1\n1\n100000\n0\n"
                  "(10 0\n";
        for (int box = 0; box < 10; ++box) {
            stream << "((" << box << ",0) (" << box << ",3) (0,0))\n";
        }
        stream << ")\n10\n";
        for (int box = 0; box < 10; ++box) {
            stream << "FabOnDisk: Cell_D_00000 " << box * 4096 << '\n';
        }
        stream << "\n10,100000\n";  // shape matches; no values follow
    }
    requireBoundedAllocation(
        [&matrix] { (void)amrvis::detail::readVisMfIndex(matrix, 2); },
        "a statistics matrix whose declared shape has no values behind it");

    // The first line of the Header, which is where the session used to re-read
    // the file version with an unbounded getline *before* the bounded parser
    // saw anything. The remote server's open path ran that read, so the
    // crafted-Header case in test_remote_server could not see it: the refusal
    // message was identical whether or not the file had been slurped first.
    // Measuring is what distinguishes them, so it is measured here.
    const auto firstLine = scratch / "long_first_line";
    std::filesystem::create_directories(firstLine);
    {
        std::ofstream stream(firstLine / "Header", std::ios::binary);
        stream << std::string(4u * 1024u * 1024u, 'V') << "\n1\ndensity\n";
    }
    // Through LocalDatasetSession, not the reader: the reader bounds the
    // version token either way, and it was the session that opened the Header a
    // second time to re-read that line. Calling the reader here would measure
    // the wrong path and pass whether or not the second read exists.
    requireBoundedAllocation(
        [&firstLine] {
            (void)amrvis::LocalDatasetSession(firstLine, amrvis::DatasetId{1},
                16ULL * 1024ULL * 1024ULL);
        },
        "a Header whose first line is a four-megabyte run");

    // The top-level Header's numeric fields: `double` extraction buffers the
    // whole digit run, so a long one amplifies by ~3.2x before it even fails.
    const auto digits = scratch / "long_double_run";
    std::filesystem::create_directories(digits);
    {
        std::ofstream stream(digits / "Header", std::ios::binary);
        stream << "HyperCLaw-V1.1\n1\ndensity\n2\n" << std::string(4u * 1024u * 1024u, '9')
               << '\n';
    }
    requireBoundedAllocation(
        [&digits] { (void)amrvis::PlotfileMetadataReader{}.read(digits); },
        "a Header whose time field is a four-megabyte digit run");

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " header_allocation_bounds test failure(s)\n";
        return 1;
    }
    return 0;
}
