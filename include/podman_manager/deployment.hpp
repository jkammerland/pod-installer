#pragma once

#include "podman_manager/error.hpp"
#include "podman_manager/podman_client.hpp"
#include "podman_manager/quadlet.hpp"
#include "podman_manager/socket_validation.hpp"
#include "podman_manager/systemd.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>

namespace podman_manager
{
struct ImageArchive
{
    std::filesystem::path path;
    std::string expected_sha256;
};

struct DeploymentBundle
{
    uid_t target_uid{};
    std::string service_name;
    std::optional<ImageArchive> image_archive;
    QuadletFile quadlet;
    std::string revision;
};

struct DeploymentOptions
{
    RuntimeDirectoryLayout runtime_layout{};
    std::string api_version{"5.0.0"};
    bool validate_socket{true};
    bool load_image_archive{true};
    bool restart_unit{true};
    bool dry_run{false};
};

struct DeploymentResult
{
    std::filesystem::path installed_quadlet_path;
    std::string systemd_unit;
    std::string job_path;
    std::optional<UnitStatus> status;
    bool dry_run{};
};

Result<void> validate_deployment_bundle(const DeploymentBundle& bundle);

class DeploymentOrchestrator
{
public:
    DeploymentOrchestrator(QuadletInstaller installer,
                           UserSystemdController& systemd,
                           DeploymentOptions options = {});

    Result<DeploymentResult> deploy(const DeploymentBundle& bundle) const;

private:
    QuadletInstaller installer_;
    UserSystemdController* systemd_{};
    DeploymentOptions options_;
};
}

