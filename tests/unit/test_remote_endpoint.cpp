#include "RemoteEndpoint.hpp"

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
    using amrvis::qt::parseRemoteEndpoint;

    const auto ipv4 = parseRemoteEndpoint("127.0.0.1:48192");
    require(ipv4 && ipv4->host == "127.0.0.1" && ipv4->port == 48192
            && ipv4->token.empty(),
        "IPv4 endpoint was rejected");
    const auto hostname = parseRemoteEndpoint("login.example.org:22");
    require(hostname && hostname->host == "login.example.org"
            && hostname->port == 22,
        "hostname endpoint was rejected");
    const auto ipv6 = parseRemoteEndpoint("[::1]:48192");
    require(ipv6 && ipv6->host == "::1" && ipv6->port == 48192,
        "bracketed IPv6 endpoint was rejected");
    const auto token = parseRemoteEndpoint(
        " [2001:db8::1]:48192 # session:token ");
    require(token && token->host == "2001:db8::1"
            && token->port == 48192 && token->token == "session:token",
        "token-bearing bracketed IPv6 endpoint was rejected");

    require(!parseRemoteEndpoint("::1:48192"),
        "ambiguous bare IPv6 endpoint was accepted");
    require(!parseRemoteEndpoint("host")
            && !parseRemoteEndpoint("host:")
            && !parseRemoteEndpoint(":48192"),
        "missing host or port was accepted");
    require(!parseRemoteEndpoint("[::1]48192")
            && !parseRemoteEndpoint("host]:48192")
            && !parseRemoteEndpoint("[::1]:0")
            && !parseRemoteEndpoint("[::1]:65536"),
        "malformed bracketed endpoint was accepted");
    return 0;
}
