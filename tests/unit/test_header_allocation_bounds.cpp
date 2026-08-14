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

    // Everything before this point -- iostreams, the filesystem calls, the
    // static initializers -- is excluded by taking the baseline here.
    const auto before = g_allocatedBytes.load(std::memory_order_relaxed);
    bool threw = false;
    try {
        (void)amrvis::detail::readVisMfIndex(path, 2);
    } catch (const amrvis::MetadataReadError&) {
        threw = true;
    } catch (const std::exception& other) {
        std::cerr << "FAILED: the crafted Header threw the wrong exception: "
                  << other.what() << '\n';
        ++g_failures;
    }
    const auto used = g_allocatedBytes.load(std::memory_order_relaxed) - before;

    require(threw, "a BoxArray shorter than its declared count was not rejected");
    // Two orders of magnitude of headroom over what the parse actually needs
    // (a handful of small vectors and the stream buffer), and two below what
    // the unbounded reserve would take.
    constexpr std::size_t allowedBytes = 8U * 1024U * 1024U;
    if (used >= allowedBytes) {
        std::cerr << "FAILED: parsing a 42-byte Header that claims ten million "
                     "boxes allocated " << used << " bytes; the reserve is not "
                     "bounded by the file's size\n";
        ++g_failures;
    }

    std::error_code removeError;
    std::filesystem::remove_all(scratch, removeError);

    if (g_failures != 0) {
        std::cerr << g_failures << " header_allocation_bounds test failure(s)\n";
        return 1;
    }
    return 0;
}
