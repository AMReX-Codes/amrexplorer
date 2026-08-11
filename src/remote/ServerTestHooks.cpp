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
    return std::min(requested, hook(requested));
}

ResponseWriteScope::ResponseWriteScope() noexcept
{
    inResponseWrite = true;
}

ResponseWriteScope::~ResponseWriteScope()
{
    inResponseWrite = false;
}

} // namespace amrvis::remote::testing
