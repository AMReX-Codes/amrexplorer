#include <amrexplorer/cache/ByteLruCache.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    amrvis::ByteLruCache<int, std::string> cache(100);

    auto first = cache.insertAndPin(1, std::make_shared<const std::string>("first"), 60);
    require(*first == "first", "inserted value mismatch");
    require(cache.metrics().residentBytes == 60, "resident accounting mismatch");
    require(cache.metrics().pinnedBytes == 60, "pinned accounting mismatch");
    first = {};
    require(cache.metrics().pinnedBytes == 0, "released handle remained pinned");

    auto second = cache.insertAndPin(2, std::make_shared<const std::string>("second"), 50);
    require(!cache.findAndPin(1), "least-recently-used entry was not evicted");
    require(cache.metrics().evictions == 1, "eviction count mismatch");

    bool pinnedFailure = false;
    try {
        [[maybe_unused]] auto third = cache.insertAndPin(
            3, std::make_shared<const std::string>("third"), 60);
    } catch (const amrvis::CacheBudgetExceeded&) {
        pinnedFailure = true;
    }
    require(pinnedFailure, "insertion displaced a pinned entry");

    second = {};
    auto third = cache.insertAndPin(3, std::make_shared<const std::string>("third"), 60);
    auto thirdAgain = cache.findAndPin(3);
    require(thirdAgain && *thirdAgain == "third", "cache hit failed");
    require(cache.metrics().pinnedBytes == 60, "multiply pinned bytes were double-counted");

    require(!cache.setBudget(30), "budget reduction ignored a pinned entry");
    require(cache.metrics().residentBytes == 60, "pinned entry was evicted");
    third = {};
    require(cache.metrics().pinnedBytes == 60, "one of two pins released the entry");
    thirdAgain = {};
    require(cache.metrics().pinnedBytes == 0, "final pin did not release the entry");
    require(cache.setBudget(30), "released entry prevented budget enforcement");
    require(cache.metrics().residentBytes == 0, "budget enforcement did not evict entry");

    bool oversizeFailure = false;
    try {
        [[maybe_unused]] auto oversize = cache.insertAndPin(
            4, std::make_shared<const std::string>("oversize"), 31);
    } catch (const amrvis::CacheBudgetExceeded&) {
        oversizeFailure = true;
    }
    require(oversizeFailure, "oversize entry was accepted");
    require(cache.metrics().withinBudget(), "cache ended outside its byte budget");

    const auto make = [](const char* text) {
        return std::make_shared<const std::string>(text);
    };

    // --- hit/miss accounting: peekAndPin records neither ------------------
    amrvis::ByteLruCache<int, std::string> metrics(100);
    require(!metrics.peekAndPin(1), "peek on an absent key returned a value");
    require(metrics.metrics().misses == 0 && metrics.metrics().hits == 0,
        "peekAndPin recorded a hit or miss");
    require(!metrics.findAndPin(1), "find on an absent key returned a value");
    require(metrics.metrics().misses == 1, "a single miss was not counted once");

    auto pinned = metrics.insertAndPin(1, make("a"), 40);
    if (auto peeked = metrics.peekAndPin(1)) {
        require(metrics.metrics().hits == 0, "peekAndPin recorded a hit");
    } else {
        require(false, "peekAndPin missed a present key");
    }
    if (auto hit = metrics.findAndPin(1)) {
        require(metrics.metrics().hits == 1, "findAndPin hit was not counted");
    } else {
        require(false, "findAndPin missed a present key");
    }
    require(metrics.metrics().misses == 1, "a hit inflated the miss count");

    // --- clearUnpinned counts clears, not evictions ----------------------
    auto other = metrics.insertAndPin(2, make("b"), 40);
    pinned = {};
    other = {};
    const auto beforeClear = metrics.metrics();
    metrics.clearUnpinned();
    const auto afterClear = metrics.metrics();
    require(afterClear.residentBytes == 0, "clearUnpinned left entries resident");
    require(afterClear.clears == beforeClear.clears + 2,
        "clearUnpinned did not count two clears");
    require(afterClear.evictions == beforeClear.evictions,
        "clearUnpinned inflated the eviction count");

    // --- a capacity eviction counts an eviction, not a clear -------------
    auto resident = metrics.insertAndPin(3, make("c"), 60);
    resident = {};
    const auto beforeEvict = metrics.metrics();
    auto incoming = metrics.insertAndPin(4, make("d"), 60);
    require(metrics.metrics().evictions == beforeEvict.evictions + 1,
        "a capacity eviction was not counted");
    require(metrics.metrics().clears == beforeEvict.clears,
        "a capacity eviction inflated the clear count");
    incoming = {};

    // --- evictFor sweeps past a pinned LRU tail --------------------------
    // Entry 1 is the least-recently-used (list tail) but pinned; the single
    // rbegin->rend sweep must skip it and evict the newer unpinned entry 2.
    amrvis::ByteLruCache<int, std::string> sweep(100);
    auto pinnedTail = sweep.insertAndPin(1, make("pinned"), 40);
    auto evictable = sweep.insertAndPin(2, make("evictable"), 40);
    evictable = {};
    auto big = sweep.insertAndPin(3, make("big"), 40);
    require(!sweep.findAndPin(2), "the unpinned entry was not evicted");
    if (auto survivor = sweep.findAndPin(1)) {
        require(*survivor == "pinned", "wrong entry survived the sweep");
    } else {
        require(false, "the pinned tail entry was wrongly evicted");
    }
    return 0;
}

