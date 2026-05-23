#include "podman_manager/systemd.hpp"

#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace podman_manager
{
namespace
{
bool safe_systemd_value(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    for (const unsigned char c : value)
    {
        if (c < 0x20 || c == 0x7f)
        {
            return false;
        }
    }
    return true;
}

Result<void> run_process(const std::vector<std::string>& args)
{
    if (args.empty())
    {
        return std::unexpected(make_error(ErrorKind::systemd, "no process arguments supplied"));
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid{};
    const int spawn_rc = posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_rc != 0)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "posix_spawnp failed for '" + args.front() + "': " +
                                              std::strerror(spawn_rc),
                                          0,
                                          spawn_rc));
    }

    int status{};
    if (waitpid(pid, &status, 0) < 0)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "waitpid failed for systemctl: " + std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "systemctl exited with status " + std::to_string(status)));
    }

    return {};
}
}

Result<void> validate_user_slice_policy(const UserSlicePolicy& policy)
{
    const bool any = policy.cpu_quota || policy.cpu_weight || policy.memory_max || policy.tasks_max ||
                     policy.allowed_cpus;
    if (!any)
    {
        return std::unexpected(make_error(ErrorKind::policy, "user slice policy contains no properties"));
    }

    if (policy.cpu_quota && !safe_systemd_value(*policy.cpu_quota))
    {
        return std::unexpected(make_error(ErrorKind::policy, "CPUQuota value is empty or contains control chars"));
    }
    if (policy.cpu_weight && (*policy.cpu_weight < 1 || *policy.cpu_weight > 10000))
    {
        return std::unexpected(make_error(ErrorKind::policy, "CPUWeight must be between 1 and 10000"));
    }
    if (policy.memory_max && !safe_systemd_value(*policy.memory_max))
    {
        return std::unexpected(make_error(ErrorKind::policy, "MemoryMax value is empty or contains control chars"));
    }
    if (policy.tasks_max && *policy.tasks_max == 0)
    {
        return std::unexpected(make_error(ErrorKind::policy, "TasksMax must be positive"));
    }
    if (policy.allowed_cpus && !safe_systemd_value(*policy.allowed_cpus))
    {
        return std::unexpected(make_error(ErrorKind::policy, "AllowedCPUs value is empty or contains control chars"));
    }

    return {};
}

Result<std::vector<std::string>> build_systemctl_set_property_args(const UserSlicePolicy& policy)
{
    if (auto result = validate_user_slice_policy(policy); !result)
    {
        return std::unexpected(result.error());
    }

    std::vector<std::string> args{
        "systemctl",
        "set-property",
        "--runtime",
        "user-" + std::to_string(policy.uid) + ".slice",
    };
    if (policy.cpu_quota)
    {
        args.push_back("CPUQuota=" + *policy.cpu_quota);
    }
    if (policy.cpu_weight)
    {
        args.push_back("CPUWeight=" + std::to_string(*policy.cpu_weight));
    }
    if (policy.memory_max)
    {
        args.push_back("MemoryMax=" + *policy.memory_max);
    }
    if (policy.tasks_max)
    {
        args.push_back("TasksMax=" + std::to_string(*policy.tasks_max));
    }
    if (policy.allowed_cpus)
    {
        args.push_back("AllowedCPUs=" + *policy.allowed_cpus);
    }

    return args;
}

SystemctlSliceController::SystemctlSliceController(bool dry_run)
    : dry_run_{dry_run}
{
}

bool SystemctlSliceController::dry_run() const noexcept
{
    return dry_run_;
}

Result<void> SystemctlSliceController::apply(const UserSlicePolicy& policy) const
{
    auto args = build_systemctl_set_property_args(policy);
    if (!args)
    {
        return std::unexpected(args.error());
    }
    if (dry_run_)
    {
        return {};
    }
    return run_process(*args);
}
}
