#include <amrexplorer/core/StopToken.hpp>

#include <cstdlib>
#include <iostream>

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
    amrvis::StopToken empty;
    require(!empty.stop_possible(), "default token should not be stoppable");
    require(!empty.stop_requested(), "default token should not be stopped");

    amrvis::StopSource source;
    const auto token = source.get_token();
    require(source.stop_possible(), "source should be stoppable");
    require(token.stop_possible(), "source token should be stoppable");
    require(!token.stop_requested(), "new source token should not be stopped");
    require(source.request_stop(), "first stop request should succeed");
    require(source.stop_requested(), "source should report requested stop");
    require(token.stop_requested(), "token should observe requested stop");
    require(!source.request_stop(), "second stop request should report no change");
}
