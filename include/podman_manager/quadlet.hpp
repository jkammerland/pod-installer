#pragma once

#include "podman_manager/container_spec.hpp"
#include "podman_manager/error.hpp"
#include "podman_manager/systemd.hpp"

#include <optional>
#include <string>
#include <vector>

namespace podman_manager
{
struct QuadletContainer
{
    std::string description;
    ContainerSpec container;
    std::optional<UserSlicePolicy> service_policy;
    std::string restart{"on-failure"};
    std::vector<std::string> after_units{"network-online.target"};
};

Result<std::string> render_quadlet_container(const QuadletContainer& unit);
}

