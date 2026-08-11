#include "ServerTestHooks.hpp"

#include <mutex>
#include <utility>

namespace amrvis::remote::testing {
namespace {

std::mutex hookMutex;
BeforeResponseWrite beforeResponseWrite;

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

} // namespace amrvis::remote::testing
