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
    return 0;
}

