#include <amrexplorer/remote/MemoryLimit.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

namespace amrvis::remote {
namespace {
std::optional<std::uint64_t> number(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}
std::optional<std::uint64_t> multiply(std::uint64_t a, std::uint64_t b)
{
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) {
        return std::nullopt;
    }
    return a * b;
}
bool contains(const std::string& list, const std::string& item)
{
    return ("," + list + ",").find("," + item + ",") != std::string::npos;
}
std::string unescape(const std::string& text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7' &&
            text[i + 2] >= '0' && text[i + 2] <= '7' && text[i + 3] >= '0' && text[i + 3] <= '7') {
            result += static_cast<char>((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 +
                                        text[i + 3] - '0');
            i += 3;
        } else {
            result += text[i];
        }
    }
    return result;
}
std::optional<std::string> readFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    std::string result;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        result.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (result.size() > 1024U * 1024U) {
            return std::nullopt;
        }
    }
    return result;
}
} // namespace

std::optional<MemoryLimit> detectMemoryLimit(const MemoryLimitInputs& inputs)
{
    std::optional<MemoryLimit> result;
    const auto bound = [&result](std::uint64_t bytes, const std::string& source) {
        if (!result || bytes < result->bytes) {
            result = MemoryLimit{bytes, source};
        }
    };
    std::istringstream membership(inputs.cgroupMembership);
    for (std::string line; std::getline(membership, line);) {
        const auto first = line.find(':');
        const auto second = line.find(':', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            continue;
        }
        const auto controllers = line.substr(first + 1, second - first - 1);
        const bool v2 = controllers.empty();
        if (!v2 && !contains(controllers, "memory")) {
            continue;
        }
        const std::filesystem::path group = line.substr(second + 1);
        if (!group.is_absolute()) {
            continue;
        }
        std::istringstream mounts(inputs.mountInfo);
        for (std::string mount; std::getline(mounts, mount);) {
            const auto separator = mount.find(" - ");
            if (separator == std::string::npos) {
                continue;
            }
            std::istringstream tail(mount.substr(separator + 3));
            std::string type, device, options;
            tail >> type >> device >> options;
            if (type != (v2 ? "cgroup2" : "cgroup") || (!v2 && !contains(options, "memory"))) {
                continue;
            }
            std::istringstream head(mount.substr(0, separator));
            std::string id, parent, majorMinor, rootText, mountText;
            if (!(head >> id >> parent >> majorMinor >> rootText >> mountText)) {
                continue;
            }
            const std::filesystem::path root = unescape(rootText);
            const std::filesystem::path mountPoint = unescape(mountText);
            if (!root.is_absolute() || !mountPoint.is_absolute()) {
                continue;
            }
            const auto relative = group.lexically_relative(root);
            if (relative.empty() ||
                std::find(relative.begin(), relative.end(), "..") != relative.end()) {
                continue;
            }
            auto directory = (mountPoint / relative).lexically_normal();
            // The task may be unlimited while a job/step ancestor is limited.
            for (;;) {
                const auto path = directory / (v2 ? "memory.max" : "memory.limit_in_bytes");
                if (const auto content = inputs.readFile(path.string())) {
                    if (const auto bytes = number(*content);
                        bytes && (v2 || *bytes < (1ULL << 60U))) {
                        bound(*bytes, path.string());
                    }
                }
                if (directory == mountPoint || directory == directory.root_path()) {
                    break;
                }
                directory = directory.parent_path();
            }
        }
    }
    const auto envNumber = [&inputs](const std::string& name) -> std::optional<std::uint64_t> {
        const auto value = inputs.environment(name);
        return value ? number(*value) : std::nullopt;
    };
    bool allNodeMemory = false;
    if (const auto mib = envNumber("SLURM_MEM_PER_NODE")) {
        allNodeMemory = *mib == 0;
        if (const auto bytes = multiply(*mib, 1024ULL * 1024ULL); bytes && *bytes) {
            bound(*bytes, "SLURM_MEM_PER_NODE");
        }
    }
    if (const auto mib = envNumber("SLURM_MEM_PER_CPU")) {
        // CPUs assigned on this node, not total CPUs across the allocation.
        if (const auto cpus = envNumber("SLURM_CPUS_ON_NODE")) {
            if (const auto total = multiply(*mib, *cpus)) {
                if (const auto bytes = multiply(*total, 1024ULL * 1024ULL); bytes && *bytes) {
                    bound(*bytes, "SLURM_MEM_PER_CPU * SLURM_CPUS_ON_NODE");
                }
            }
        }
    }
    // A Slurm job with no discoverable bound must not inherit the full RAM of
    // a shared node. --mem=0 explicitly requests all node memory.
    const bool slurm = inputs.environment("SLURM_JOB_ID").has_value() ||
                       inputs.environment("SLURM_JOBID").has_value();
    if (inputs.physicalBytes && (result || !slurm || allNodeMemory)) {
        bound(*inputs.physicalBytes, "physical memory");
    }
    return result;
}

std::optional<MemoryLimit> detectMemoryLimit()
{
    MemoryLimitInputs inputs;
    inputs.readFile = readFile;
    inputs.environment = [](const std::string& name) -> std::optional<std::string> {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t size = 0;
        const auto error = _dupenv_s(&value, &size, name.c_str());
        const std::unique_ptr<char, decltype(&std::free)> ownedValue(value, &std::free);
        if (error != 0 || !ownedValue) {
            return std::nullopt;
        }
        return std::string(ownedValue.get());
#else
        const auto* value = std::getenv(name.c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
#endif
    };
#ifdef __linux__
    inputs.cgroupMembership = readFile("/proc/self/cgroup").value_or("");
    inputs.mountInfo = readFile("/proc/self/mountinfo").value_or("");
    const auto pages = ::sysconf(_SC_PHYS_PAGES);
    const auto pageSize = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        inputs.physicalBytes =
            multiply(static_cast<std::uint64_t>(pages), static_cast<std::uint64_t>(pageSize));
    }
#endif
    return detectMemoryLimit(inputs);
}

double parseCacheMemoryFraction(const std::string& text)
{
    double value = 0;
    // Older Apple libc++ versions lack floating-point from_chars. Keep parsing
    // locale-independent and reject whitespace, hex, leading '+', and trailing text.
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    if (text.empty() || text.front() == '+' ||
        text.find_first_not_of("0123456789.eE+-") != std::string::npos ||
        input.fail() || !input.eof() ||
        !std::isfinite(value) || value <= 0 || value > 1) {
        throw std::invalid_argument("--cache-memory-fraction must be greater than 0 and at most 1");
    }
    return value;
}

std::uint64_t cacheBytesForFraction(std::uint64_t bytes, double fraction)
{
    if (!std::isfinite(fraction) || fraction <= 0 || fraction > 1) {
        throw std::invalid_argument("invalid cache memory fraction");
    }
    // Avoid an out-of-range conversion when uint64_max rounds up in double.
    if (fraction == 1) {
        return bytes;
    }
    const auto scaled = static_cast<long double>(bytes) * fraction;
    return static_cast<std::uint64_t>(scaled);
}
} // namespace amrvis::remote
