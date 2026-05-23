#pragma once

#include "podman_manager/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace podman_manager
{
struct UserSlicePolicy
{
    uid_t uid{};
    std::optional<std::string> cpu_quota;
    std::optional<uint64_t> cpu_weight;
    std::optional<std::string> memory_max;
    std::optional<uint64_t> tasks_max;
    std::optional<std::string> allowed_cpus;
};

Result<void> validate_user_slice_policy(const UserSlicePolicy& policy);
Result<std::vector<std::string>> build_systemctl_set_property_args(const UserSlicePolicy& policy);

class SystemctlSliceController
{
public:
    explicit SystemctlSliceController(bool dry_run = false);

    [[nodiscard]] bool dry_run() const noexcept;
    Result<void> apply(const UserSlicePolicy& policy) const;

private:
    bool dry_run_{};
};
}

