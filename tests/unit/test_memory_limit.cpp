#include <amrexplorer/remote/MemoryLimit.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <stdexcept>

namespace {
class CommaDecimalPoint : public std::numpunct<char> {
protected:
    char do_decimal_point() const override { return ','; }
};

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
    using namespace amrvis::remote;
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    std::map<std::string, std::string> files, env;
    MemoryLimitInputs inputs;
    inputs.readFile = [&files](const std::string& path) -> std::optional<std::string> {
        const auto it = files.find(path);
        return it == files.end() ? std::nullopt : std::optional(it->second);
    };
    inputs.environment = [&env](const std::string& name) -> std::optional<std::string> {
        const auto it = env.find(name);
        return it == env.end() ? std::nullopt : std::optional(it->second);
    };
    inputs.physicalBytes = 1024 * mib;
    require(detectMemoryLimit(inputs)->bytes == 1024 * mib, "physical fallback");
    env["SLURM_JOB_ID"] = "123";
    require(!detectMemoryLimit(inputs), "Slurm must not inherit the whole node");
    env["SLURM_MEM_PER_NODE"] = "512";
    require(detectMemoryLimit(inputs)->bytes == 512 * mib, "Slurm node memory units");
    env["SLURM_MEM_PER_CPU"] = "32";
    env["SLURM_CPUS_ON_NODE"] = "4";
    require(detectMemoryLimit(inputs)->bytes == 128 * mib, "Slurm per-CPU bound");
    env["SLURM_CPUS_ON_NODE"] = "4(x2)";
    require(detectMemoryLimit(inputs)->bytes == 512 * mib, "malformed CPU count ignored");
    env.erase("SLURM_MEM_PER_CPU");
    inputs.cgroupMembership = "0::/jobs/123/step/task\n";
    inputs.mountInfo = "29 23 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n";
    files["/sys/fs/cgroup/jobs/123/step/task/memory.max"] = "max\n";
    files["/sys/fs/cgroup/jobs/123/memory.max"] = std::to_string(256 * mib);
    require(detectMemoryLimit(inputs)->bytes == 256 * mib, "v2 ancestor limit");
    files["/sys/fs/cgroup/jobs/123/step/memory.max"] = std::to_string(64 * mib);
    require(detectMemoryLimit(inputs)->bytes == 64 * mib, "tightest v2 limit");
    files["/sys/fs/cgroup/jobs/123/step/memory.max"] = "0";
    require(detectMemoryLimit(inputs)->bytes == 0, "zero cgroup limit is binding");
    files.clear();
    inputs.mountInfo = "29 23 0:26 /jobs/123 /mounted\\040group rw - cgroup2 cgroup rw\n";
    files["/mounted group/memory.max"] = "1000\n";
    require(detectMemoryLimit(inputs)->bytes == 1000, "subtree mount and escaped path");
    inputs.cgroupMembership = "0::/jobs/1234/task\n";
    require(detectMemoryLimit(inputs)->bytes == 512 * mib, "mount root requires path boundary");
    inputs.cgroupMembership = "5:cpu,memory:/jobs/123/task\n";
    inputs.mountInfo = "29 23 0:26 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n";
    files["/sys/fs/cgroup/memory/jobs/123/task/memory.limit_in_bytes"] = "9223372036854771712\n";
    files["/sys/fs/cgroup/memory/jobs/123/memory.limit_in_bytes"] = "4096\n";
    require(detectMemoryLimit(inputs)->bytes == 4096, "v1 ancestor with unlimited leaf");
    inputs.cgroupMembership.clear();
    env["SLURM_MEM_PER_NODE"] = "18446744073709551615";
    require(!detectMemoryLimit(inputs), "overflow is not a memory budget");
    env["SLURM_MEM_PER_NODE"] = "0";
    require(detectMemoryLimit(inputs)->bytes == 1024 * mib, "explicit all-node memory");
    inputs.physicalBytes.reset();
    env.clear();
    require(!detectMemoryLimit(inputs), "unavailable detection");
    const auto previousLocale =
        std::locale::global(std::locale(std::locale::classic(), new CommaDecimalPoint));
    for (const auto* invalid : {"", "0", "-0.5", "1.01", "nan", "inf", "0.5junk",
                                " 0.5", "0.5 ", "\t0.5", "0.5\n", "+0.5", "0,5",
                                "0x1p-1", "1e", "1e9999", "1e-9999"}) {
        bool rejected = false;
        try {
            static_cast<void>(parseCacheMemoryFraction(invalid));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected) {
            std::cerr << "Invalid fraction: '" << invalid << "'\n";
        }
        require(rejected, "invalid fraction accepted");
    }
    require(cacheBytesForFraction(1024, parseCacheMemoryFraction("0.5")) == 512,
            "fraction scaling");
    require(parseCacheMemoryFraction("5e-1") == 0.5, "scientific notation fraction");
    require(parseCacheMemoryFraction(".5") == 0.5, "fraction without leading zero");
    require(parseCacheMemoryFraction("1") == 1, "full fraction");
    std::locale::global(previousLocale);
    require(cacheBytesForFraction(std::numeric_limits<std::uint64_t>::max(), 1) ==
                std::numeric_limits<std::uint64_t>::max(),
            "full uint64 budget");
    require(cacheBytesForFraction(1, 0.5) == 0, "tiny allowance rounds down");
}
