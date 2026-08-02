#pragma once

#include <amrexplorer/remote/Frame.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace amrvis::remote {

struct ServerOptions {
    std::uint16_t port = 0;
    unsigned int workerCount = 0;
    std::uint32_t maximumFrameBytes = defaultMaximumFrameBytes;
    std::uint32_t maximumDatasets = 8;
    std::uint32_t maximumOutstandingRequests = 64;
    std::uint32_t maximumConnections = 32;
    std::string softwareVersion = "unknown";
};

class Server {
public:
    explicit Server(ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string lastError() const;
    void run();
    void requestStop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace amrvis::remote
