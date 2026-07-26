#include <amrexplorer/cache/ByteLruCache.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

    // --- erase(): false for an absent or pinned key, true (and one clear) for
    // an unpinned one; a failed erase changes nothing.
    amrvis::ByteLruCache<int, std::string> eraser(100);
    require(!eraser.erase(99), "erase reported success for an absent key");
    auto held = eraser.insertAndPin(1, make("held"), 40);
    require(!eraser.erase(1), "erase removed a pinned entry");
    require(eraser.metrics().residentBytes == 40, "a failed erase changed residency");
    require(eraser.metrics().clears == 0, "a failed erase counted a clear");
    held = {};
    const auto beforeErase = eraser.metrics();
    require(eraser.erase(1), "erase failed on an unpinned entry");
    require(eraser.metrics().residentBytes == 0, "erase left the entry resident");
    require(eraser.metrics().clears == beforeErase.clears + 1,
        "a successful erase did not count a clear");
    require(eraser.metrics().evictions == beforeErase.evictions,
        "erase inflated the eviction count");
    require(!eraser.findAndPin(1), "the erased entry was still present");

    // --- hitRate(): 0 with no accesses, hits/(hits+misses) otherwise.
    amrvis::ByteLruCache<int, std::string> rate(100);
    require(rate.metrics().hitRate() == 0.0, "the empty-cache hit rate is not zero");
    require(!rate.findAndPin(1), "find on an absent key returned a value");
    require(!rate.findAndPin(2), "find on an absent key returned a value");
    require(rate.metrics().hitRate() == 0.0, "the all-miss hit rate is not zero");
    auto rateHeld = rate.insertAndPin(1, make("x"), 10);  // a new key: no hit/miss
    (void)rate.findAndPin(1);
    (void)rate.findAndPin(1);
    require(rate.metrics().hits == 2 && rate.metrics().misses == 2,
        "the hit/miss tally for the ratio is wrong");
    require(rate.metrics().hitRate() == 0.5,
        "the hit rate is not hits / (hits + misses)");
    rateHeld = {};

    // --- LRU ordering with more than two entries: touching an entry moves it
    // to the front, so a later eviction drops the genuinely-oldest untouched
    // entry, not the touched one. Order after inserts (tail->head): 1, 2, 3.
    amrvis::ByteLruCache<int, std::string> order(100);
    auto a = order.insertAndPin(1, make("a"), 30);
    auto b = order.insertAndPin(2, make("b"), 30);
    auto c = order.insertAndPin(3, make("c"), 30);
    a = {};
    b = {};
    c = {};
    { auto touched = order.findAndPin(1); }  // 1 -> front, so 2 is now the tail
    auto d = order.insertAndPin(4, make("d"), 30);  // 90 + 30 > 100: evict tail (2)
    require(!order.findAndPin(2), "the least-recently-used entry was not evicted");
    require(bool(order.findAndPin(1)), "a recently-touched entry was wrongly evicted");
    require(bool(order.findAndPin(3)), "a mid-list entry was wrongly evicted");
    d = {};

    // --- concurrent stress: many threads insert/find/erase against one cache.
    // The mutex must keep every operation consistent (this is the case the TSan
    // build watches); afterward no pins remain and the budget still holds.
    {
        amrvis::ByteLruCache<int, std::string> shared(4000);
        constexpr int threadCount = 8;
        constexpr int iterations = 5000;
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (int t = 0; t < threadCount; ++t) {
            workers.emplace_back([&shared, t] {
                for (int i = 0; i < iterations; ++i) {
                    const int key = (t * 31 + i) % 128;
                    if (i % 4 == 0) {
                        (void)shared.erase(key);
                    } else if (auto found = shared.findAndPin(key)) {
                        (void)found;  // hold the pin to end of iteration
                    } else {
                        try {
                            auto handle = shared.insertAndPin(key,
                                std::make_shared<const std::string>(
                                    std::to_string(key)), 100);
                            (void)handle;
                        } catch (const amrvis::CacheBudgetExceeded&) {
                            // Transient pin pressure is an acceptable outcome.
                        }
                    }
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        const auto snapshot = shared.metrics();
        require(snapshot.pinnedBytes == 0,
            "a handle outlived its scope in the stress test");
        require(snapshot.withinBudget(),
            "the cache exceeded its budget under concurrency");
        require(snapshot.hits + snapshot.misses > 0,
            "the stress test recorded no cache accesses");
    }

    return 0;
}

