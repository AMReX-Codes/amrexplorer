#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace amrvis::remote::testing {

// Internal synchronization seam, compiled only into test-enabled builds.
using BeforeResponseWrite = std::function<void(std::uint64_t)>;

void setBeforeResponseWrite(BeforeResponseWrite hook);
void clearBeforeResponseWrite();
void notifyBeforeResponseWrite(std::uint64_t requestId);

// Pacing seam for the frame write itself. The hook is handed the bytes the
// write would like to attempt next and returns how many it may; it is free to
// block first. A hook that returns a few bytes slowly is a trickle -- progress
// that renews the no-progress deadline forever, so only the whole-response
// budget can retire it -- and it produces that condition from a tiny response
// instead of one large enough to exhaust the kernel's socket buffers.
// The result is clamped into [1, requested] whenever requested is non-zero, so
// a hook cannot accidentally stall the write outright by answering zero. A
// zero request -- which the write loop never makes -- answers zero.
using WriteChunkLimit = std::function<std::size_t(std::size_t)>;

void setWriteChunkLimit(WriteChunkLimit hook);
void clearWriteChunkLimit();

// Consulted by the frame write, and only inside a ResponseWriteScope: a test
// drives its own client sockets from its own threads in this same process, and
// pacing those would throttle the test rather than the server.
[[nodiscard]] std::size_t writeChunkLimit(std::size_t requested);

class ResponseWriteScope {
public:
    ResponseWriteScope() noexcept;
    ~ResponseWriteScope();
    ResponseWriteScope(const ResponseWriteScope&) = delete;
    ResponseWriteScope& operator=(const ResponseWriteScope&) = delete;
    ResponseWriteScope(ResponseWriteScope&&) = delete;
    ResponseWriteScope& operator=(ResponseWriteScope&&) = delete;

private:
    bool m_previous = false;
};

} // namespace amrvis::remote::testing
