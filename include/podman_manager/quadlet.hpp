#pragma once

#include "podman_manager/error.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace podman_manager
{
struct QuadletFile
{
    std::string file_name;
    std::string contents;
};

struct InstalledQuadlet
{
    std::filesystem::path path;
    std::string systemd_unit;
};

struct QuadletInstallLayout
{
    std::filesystem::path admin_user_root{"/etc/containers/systemd/users"};

    [[nodiscard]] std::filesystem::path user_directory(uid_t uid) const;
    [[nodiscard]] std::filesystem::path quadlet_path(uid_t uid, std::string_view file_name) const;
};

struct QuadletPolicy
{
    bool require_container_section{true};
    bool require_image{true};
    bool require_managed_label{true};
    std::string managed_label_key{"com.example.podman-manager.managed"};
    std::string managed_label_value{"true"};
    bool allow_privileged{false};
    bool allow_host_network{false};
    bool allow_host_pid{false};
    bool allow_host_ipc{false};
    bool allow_host_userns{false};
    bool allow_devices{false};
    bool allow_root_mount{false};
};

class ParsedQuadlet
{
public:
    using Values = std::vector<std::string>;
    using Section = std::map<std::string, Values>;

    [[nodiscard]] bool has_section(std::string_view section) const;
    [[nodiscard]] std::vector<std::string> values(std::string_view section,
                                                  std::string_view key) const;

    void add(std::string section, std::string key, std::string value);

private:
    std::map<std::string, Section> sections_;
};

Result<void> validate_quadlet_file_name(std::string_view file_name);
Result<std::string> service_unit_name_from_quadlet(std::string_view file_name);
Result<ParsedQuadlet> parse_quadlet(std::string_view contents);
Result<void> validate_quadlet_policy(const QuadletFile& quadlet,
                                     const QuadletPolicy& policy = {});

class QuadletInstaller
{
public:
    explicit QuadletInstaller(QuadletInstallLayout layout = {},
                              QuadletPolicy policy = {});

    [[nodiscard]] const QuadletInstallLayout& layout() const noexcept;
    [[nodiscard]] const QuadletPolicy& policy() const noexcept;

    Result<InstalledQuadlet> expected_install(uid_t uid, const QuadletFile& quadlet) const;
    Result<InstalledQuadlet> install_for_user(uid_t uid, const QuadletFile& quadlet) const;

private:
    QuadletInstallLayout layout_;
    QuadletPolicy policy_;
};
}

