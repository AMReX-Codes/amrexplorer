#pragma once

#include <cstdint>
#include <functional>

namespace amrvis::remote::testing {

// Internal synchronization seam, compiled only into test-enabled builds.
using BeforeResponseWrite = std::function<void(std::uint64_t)>;

void setBeforeResponseWrite(BeforeResponseWrite hook);
void clearBeforeResponseWrite();
void notifyBeforeResponseWrite(std::uint64_t requestId);

} // namespace amrvis::remote::testing
