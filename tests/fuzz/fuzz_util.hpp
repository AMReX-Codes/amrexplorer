#pragma once

// Shared plumbing for the deterministic fuzz/property harnesses: a fixed-seed
// PRNG, random and near-valid (mutated) input generators, and a failure abort
// that prints a reproducer (current input as hex + the iteration context set
// by the harness loop). The harnesses are ordinary bounded ctests; run them
// under the sanitizers preset to turn UB/OOB into a failure.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace amrvis::fuzz {

// Reproducer context: the harness loop stores the iteration and the exact
// bytes it is about to feed, so fail() can print how to reproduce a failure
// instead of a bare message. Plain globals: the harnesses are single-threaded.
inline long currentIteration = -1;
inline std::vector<std::uint8_t> currentInput;

inline void setCurrentInput(long iteration, const void* data, std::size_t size)
{
    currentIteration = iteration;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    currentInput.assign(bytes, bytes + size);
}

[[noreturn]] inline void fail(const char* what)
{
    std::fprintf(stderr, "fuzz: %s\n", what);
    if (currentIteration >= 0) {
        std::fprintf(stderr, "fuzz: iteration %ld, input %zu bytes:\n",
            currentIteration, currentInput.size());
        for (std::size_t i = 0; i < currentInput.size(); ++i) {
            std::fprintf(stderr, "%02x%s", currentInput[i],
                (i + 1) % 32 == 0 ? "\n" : " ");
        }
        std::fputc('\n', stderr);
    }
    std::abort();
}

// A string_view over a byte buffer that is safe for empty input:
// string_view(nullptr, 0) violates the constructor's precondition.
inline std::string_view viewOf(const std::vector<std::uint8_t>& bytes)
{
    return bytes.empty()
        ? std::string_view{}
        : std::string_view(
              reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline std::uint64_t nextRandom(std::uint64_t& state)
{
    state += 0x9e3779b97f4a7c15ULL;
    auto z = state;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

inline std::vector<std::uint8_t> randomBytes(
    std::uint64_t& rng, std::size_t maxLen)
{
    const auto length
        = static_cast<std::size_t>(nextRandom(rng) % (maxLen + 1));
    std::vector<std::uint8_t> bytes(length);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(nextRandom(rng));
    }
    return bytes;
}

// Copy `seed` and apply one random mutation, so the input stays near a valid
// structure -- the region where boundary bugs hide.
inline std::vector<std::uint8_t> mutate(
    std::uint64_t& rng, const std::vector<std::uint8_t>& seed)
{
    auto out = seed;
    if (out.empty()) {
        return randomBytes(rng, 64);
    }
    switch (nextRandom(rng) % 4U) {
    case 0:
        out[nextRandom(rng) % out.size()]
            ^= static_cast<std::uint8_t>(1U << (nextRandom(rng) % 8U));
        break;
    case 1:
        out.resize(static_cast<std::size_t>(nextRandom(rng) % out.size()));
        break;
    case 2:
        out[nextRandom(rng) % out.size()]
            = static_cast<std::uint8_t>(nextRandom(rng));
        break;
    default:
        out.insert(
            out.begin()
                + static_cast<std::ptrdiff_t>(
                    nextRandom(rng) % (out.size() + 1)),
            static_cast<std::uint8_t>(nextRandom(rng)));
        break;
    }
    return out;
}

inline std::string mutateText(std::uint64_t& rng, const std::string& seed)
{
    std::string out = seed;
    if (out.empty()) {
        return out;
    }
    const auto rounds = 1U + static_cast<unsigned>(nextRandom(rng) % 4U);
    for (unsigned r = 0; r < rounds && !out.empty(); ++r) {
        switch (nextRandom(rng) % 4U) {
        case 0:
            // Full byte range, not % 128: high bytes are where locale/isspace
            // style footguns live, and near-valid inputs must reach them.
            out[nextRandom(rng) % out.size()]
                = static_cast<char>(nextRandom(rng) % 256U);
            break;
        case 1:
            out.resize(static_cast<std::size_t>(nextRandom(rng) % out.size()));
            break;
        case 2:
            out.insert(
                out.begin()
                    + static_cast<std::ptrdiff_t>(
                        nextRandom(rng) % (out.size() + 1)),
                static_cast<char>(nextRandom(rng) % 256U));
            break;
        default:
            out[nextRandom(rng) % out.size()]
                = "()[]{}, \n\t0123456789-"[nextRandom(rng) % 21U];
            break;
        }
    }
    return out;
}

} // namespace amrvis::fuzz
