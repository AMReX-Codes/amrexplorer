#include "ServerTestHooks.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace amrvis::remote::testing {
namespace {

std::mutex hookMutex;
BeforeResponseWrite beforeResponseWrite;
WriteChunkLimit writeChunkLimitHook;
thread_local bool inResponseWrite = false;

} // namespace

void setBeforeResponseWrite(BeforeResponseWrite hook)
{
    std::scoped_lock lock(hookMutex);
    beforeResponseWrite = std::move(hook);
}

void clearBeforeResponseWrite()
{
    setBeforeResponseWrite({});
}

void notifyBeforeResponseWrite(std::uint64_t requestId)
{
    BeforeResponseWrite hook;
    {
        std::scoped_lock lock(hookMutex);
        hook = beforeResponseWrite;
    }
    if (hook) {
        hook(requestId);
    }
}

void setWriteChunkLimit(WriteChunkLimit hook)
{
    std::scoped_lock lock(hookMutex);
    writeChunkLimitHook = std::move(hook);
}

void clearWriteChunkLimit()
{
    setWriteChunkLimit({});
}

std::size_t writeChunkLimit(std::size_t requested)
{
    if (!inResponseWrite) {
        return requested;
    }
    WriteChunkLimit hook;
    {
        std::scoped_lock lock(hookMutex);
        hook = writeChunkLimitHook;
    }
    // Copied out and invoked unlocked, like the write hook above: the hook is
    // meant to block, and holding the installer's mutex while it did would
    // deadlock whoever tries to clear it.
    if (!hook) {
        return requested;
    }
    // Zero is the dangerous answer: writeExact reads a zero-byte send as the
    // peer having closed the connection, so a hook returning zero would
    // surface as a spurious disconnect rather than as an obviously wrong test.
    // The write loop only asks while bytes remain, so requested is never zero
    // -- but clamp's precondition is lo <= hi, and an invariant held up by a
    // comment in another file is not one to hand to undefined behaviour.
    if (requested == 0) {
        return 0;
    }
    return std::clamp(hook(requested), std::size_t{1}, requested);
}

ResponseWriteScope::ResponseWriteScope() noexcept
    : m_previous(inResponseWrite)
{
    inResponseWrite = true;
}

ResponseWriteScope::~ResponseWriteScope()
{
    // Restore, not clear: an inner scope closing must not switch pacing off
    // for the remainder of an outer one.
    inResponseWrite = m_previous;
}

} // namespace amrvis::remote::testing
