#include "podman_manager/quadlet.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ranges>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace podman_manager
{
namespace
{
class FileDescriptor
{
public:
    explicit FileDescriptor(int fd = -1) noexcept
        : fd_{fd}
    {
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_{std::exchange(other.fd_, -1)}
    {
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~FileDescriptor()
    {
        reset();
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{};
};

std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.remove_suffix(1);
    }
    return std::string{value};
}

std::string lower_ascii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value)
    {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool truthy(std::string_view value)
{
    const auto normalized = lower_ascii(trim(value));
    return normalized == "1" || normalized == "yes" || normalized == "true" || normalized == "on";
}

bool contains_control_or_slash(std::string_view value)
{
    return std::ranges::any_of(value, [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '/';
    });
}

bool safe_unit_file_char(unsigned char c)
{
    return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@';
}

bool starts_with_token(std::string_view value, std::string_view token)
{
    if (value.size() < token.size())
    {
        return false;
    }
    if (lower_ascii(value.substr(0, token.size())) != lower_ascii(token))
    {
        return false;
    }
    return value.size() == token.size() || value[token.size()] == '=' || value[token.size()] == ' ';
}

bool denied_podman_arg(std::string_view value, const QuadletPolicy& policy)
{
    auto parts = std::views::split(value, ' ');
    for (const auto part_range : parts)
    {
        const std::string part{part_range.begin(), part_range.end()};
        if (part.empty())
        {
            continue;
        }
        if (!policy.allow_privileged && part == "--privileged")
        {
            return true;
        }
        if (!policy.allow_host_network &&
            (starts_with_token(part, "--network=host") || starts_with_token(part, "--net=host")))
        {
            return true;
        }
        if (!policy.allow_host_pid && starts_with_token(part, "--pid=host"))
        {
            return true;
        }
        if (!policy.allow_host_ipc && starts_with_token(part, "--ipc=host"))
        {
            return true;
        }
        if (!policy.allow_host_userns && starts_with_token(part, "--userns=host"))
        {
            return true;
        }
        if (!policy.allow_devices && starts_with_token(part, "--device"))
        {
            return true;
        }
    }
    return false;
}

bool root_mount(std::string_view value)
{
    const auto trimmed = trim(value);
    return trimmed == "/" || trimmed.starts_with("/:") || trimmed.starts_with("/:/");
}

Result<void> ensure_regular_admin_dir(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to create Quadlet directory '" + dir.string() + "': " +
                                              ec.message()));
    }

    struct stat st
    {
    };
    if (lstat(dir.c_str(), &st) != 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to stat Quadlet directory '" + dir.string() + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "Quadlet directory is not a real directory: " + dir.string()));
    }
    return {};
}

Result<void> write_all(int fd, std::string_view contents)
{
    while (!contents.empty())
    {
        const auto written = write(fd, contents.data(), contents.size());
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::unexpected(make_error(ErrorKind::filesystem,
                                              "failed to write Quadlet file: " + std::string{std::strerror(errno)},
                                              0,
                                              errno));
        }
        contents.remove_prefix(static_cast<size_t>(written));
    }
    return {};
}

Result<void> fsync_directory(const std::filesystem::path& dir)
{
    FileDescriptor fd{open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (fd.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open Quadlet directory for fsync: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }
    if (fsync(fd.get()) != 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to fsync Quadlet directory: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }
    return {};
}

Result<void> atomic_write_file(const std::filesystem::path& final_path,
                               std::string_view contents,
                               mode_t mode)
{
    const auto dir = final_path.parent_path();
    const auto tmp_path = dir / ("." + final_path.filename().string() + ".tmp");

    FileDescriptor fd{open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode)};
    if (fd.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open temporary Quadlet file '" + tmp_path.string() + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }

    if (auto result = write_all(fd.get(), contents); !result)
    {
        std::filesystem::remove(tmp_path);
        return result;
    }

    if (fsync(fd.get()) != 0)
    {
        std::filesystem::remove(tmp_path);
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to fsync temporary Quadlet file: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }

    fd.reset();
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0)
    {
        std::filesystem::remove(tmp_path);
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to install Quadlet file '" + final_path.string() + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }

    return fsync_directory(dir);
}

bool has_managed_label(const ParsedQuadlet& parsed, const QuadletPolicy& policy)
{
    for (const auto& label : parsed.values("Container", "Label"))
    {
        const auto eq = label.find('=');
        const auto key = trim(eq == std::string::npos ? label : std::string_view{label}.substr(0, eq));
        const auto value = trim(eq == std::string::npos ? "" : std::string_view{label}.substr(eq + 1));
        if (key == policy.managed_label_key && value == policy.managed_label_value)
        {
            return true;
        }
    }
    return false;
}
}

std::filesystem::path QuadletInstallLayout::user_directory(uid_t uid) const
{
    return admin_user_root / std::to_string(uid);
}

std::filesystem::path QuadletInstallLayout::quadlet_path(uid_t uid, std::string_view file_name) const
{
    return user_directory(uid) / std::string{file_name};
}

bool ParsedQuadlet::has_section(std::string_view section) const
{
    return sections_.contains(std::string{section});
}

std::vector<std::string> ParsedQuadlet::values(std::string_view section, std::string_view key) const
{
    const auto section_it = sections_.find(std::string{section});
    if (section_it == sections_.end())
    {
        return {};
    }
    const auto key_it = section_it->second.find(std::string{key});
    if (key_it == section_it->second.end())
    {
        return {};
    }
    return key_it->second;
}

void ParsedQuadlet::add(std::string section, std::string key, std::string value)
{
    sections_[std::move(section)][std::move(key)].push_back(std::move(value));
}

Result<void> validate_quadlet_file_name(std::string_view file_name)
{
    if (file_name.empty() || file_name.size() > 255)
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "Quadlet file name must be 1 to 255 bytes"));
    }
    if (file_name == "." || file_name == ".." || file_name.starts_with('.'))
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "Quadlet file name must not be hidden or relative"));
    }
    if (contains_control_or_slash(file_name))
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "Quadlet file name must not contain slashes or control characters"));
    }
    if (!std::ranges::all_of(file_name, safe_unit_file_char))
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "Quadlet file name contains unsupported characters"));
    }
    if (!file_name.ends_with(".container"))
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "MVP deployment bundles require a .container Quadlet file"));
    }
    return {};
}

Result<std::string> service_unit_name_from_quadlet(std::string_view file_name)
{
    if (auto result = validate_quadlet_file_name(file_name); !result)
    {
        return std::unexpected(result.error());
    }
    constexpr std::string_view suffix = ".container";
    return std::string{file_name.substr(0, file_name.size() - suffix.size())} + ".service";
}

Result<ParsedQuadlet> parse_quadlet(std::string_view contents)
{
    ParsedQuadlet parsed;
    std::string section;
    size_t line_no = 0;

    while (!contents.empty())
    {
        ++line_no;
        const auto newline = contents.find('\n');
        auto line = newline == std::string_view::npos ? contents : contents.substr(0, newline);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        contents = newline == std::string_view::npos ? std::string_view{} : contents.substr(newline + 1);

        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#') || trimmed.starts_with(';'))
        {
            continue;
        }
        if (trimmed.starts_with('[') && trimmed.ends_with(']'))
        {
            section = trim(std::string_view{trimmed}.substr(1, trimmed.size() - 2));
            if (section.empty())
            {
                return std::unexpected(make_error(ErrorKind::invalid_argument,
                                                  "empty section name in Quadlet at line " +
                                                      std::to_string(line_no)));
            }
            continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string::npos)
        {
            return std::unexpected(make_error(ErrorKind::invalid_argument,
                                              "expected key=value in Quadlet at line " +
                                                  std::to_string(line_no)));
        }
        if (section.empty())
        {
            return std::unexpected(make_error(ErrorKind::invalid_argument,
                                              "Quadlet key appears before any section at line " +
                                                  std::to_string(line_no)));
        }
        auto key = trim(std::string_view{trimmed}.substr(0, eq));
        auto value = trim(std::string_view{trimmed}.substr(eq + 1));
        if (key.empty())
        {
            return std::unexpected(make_error(ErrorKind::invalid_argument,
                                              "empty key in Quadlet at line " + std::to_string(line_no)));
        }
        parsed.add(section, std::move(key), std::move(value));
    }

    return parsed;
}

Result<void> validate_quadlet_policy(const QuadletFile& quadlet, const QuadletPolicy& policy)
{
    if (auto result = validate_quadlet_file_name(quadlet.file_name); !result)
    {
        return result;
    }
    if (quadlet.contents.empty())
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet contents are empty"));
    }
    if (quadlet.contents.find('\0') != std::string::npos)
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "Quadlet contents must not contain NUL bytes"));
    }

    auto parsed = parse_quadlet(quadlet.contents);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    if (policy.require_container_section && !parsed->has_section("Container"))
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "Quadlet must contain a [Container] section"));
    }
    if (policy.require_image && parsed->values("Container", "Image").empty())
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "Quadlet [Container] section must contain Image="));
    }
    if (policy.require_managed_label && !has_managed_label(*parsed, policy))
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "Quadlet must carry managed Label=" +
                                              policy.managed_label_key + "=" +
                                              policy.managed_label_value));
    }

    for (const auto& value : parsed->values("Container", "Privileged"))
    {
        if (!policy.allow_privileged && truthy(value))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet Privileged=true is not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "Network"))
    {
        if (!policy.allow_host_network && lower_ascii(trim(value)) == "host")
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet Network=host is not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "PID"))
    {
        if (!policy.allow_host_pid && lower_ascii(trim(value)) == "host")
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet PID=host is not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "IPCHost"))
    {
        if (!policy.allow_host_ipc && truthy(value))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet IPCHost=true is not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "UserNS"))
    {
        if (!policy.allow_host_userns && lower_ascii(trim(value)) == "host")
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet UserNS=host is not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "Device"))
    {
        if (!policy.allow_devices && !trim(value).empty())
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet Device= is not allowed by default"));
        }
    }
    for (const auto& value : parsed->values("Container", "Volume"))
    {
        if (!policy.allow_root_mount && root_mount(value))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet root filesystem bind mounts are not allowed"));
        }
    }
    for (const auto& value : parsed->values("Container", "PodmanArgs"))
    {
        if (denied_podman_arg(value, policy))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "Quadlet PodmanArgs contains a denied argument"));
        }
    }

    return {};
}

QuadletInstaller::QuadletInstaller(QuadletInstallLayout layout, QuadletPolicy policy)
    : layout_{std::move(layout)}
    , policy_{std::move(policy)}
{
}

const QuadletInstallLayout& QuadletInstaller::layout() const noexcept
{
    return layout_;
}

const QuadletPolicy& QuadletInstaller::policy() const noexcept
{
    return policy_;
}

Result<InstalledQuadlet> QuadletInstaller::expected_install(uid_t uid, const QuadletFile& quadlet) const
{
    if (auto result = validate_quadlet_policy(quadlet, policy_); !result)
    {
        return std::unexpected(result.error());
    }
    auto unit = service_unit_name_from_quadlet(quadlet.file_name);
    if (!unit)
    {
        return std::unexpected(unit.error());
    }
    return InstalledQuadlet{
        .path = layout_.quadlet_path(uid, quadlet.file_name),
        .systemd_unit = *unit,
    };
}

Result<InstalledQuadlet> QuadletInstaller::install_for_user(uid_t uid, const QuadletFile& quadlet) const
{
    auto expected = expected_install(uid, quadlet);
    if (!expected)
    {
        return std::unexpected(expected.error());
    }

    const auto dir = layout_.user_directory(uid);
    if (auto result = ensure_regular_admin_dir(dir); !result)
    {
        return std::unexpected(result.error());
    }
    if (auto result = atomic_write_file(expected->path, quadlet.contents, 0644); !result)
    {
        return std::unexpected(result.error());
    }

    return expected;
}
}
