#include "podman_manager/quadlet.hpp"

#include <sstream>

namespace podman_manager
{
namespace
{
bool no_newline(std::string_view value)
{
    return value.find('\n') == std::string_view::npos && value.find('\r') == std::string_view::npos;
}

Result<void> validate_line_value(std::string_view name, std::string_view value)
{
    if (!no_newline(value))
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          std::string{name} + " must not contain newline characters"));
    }
    return {};
}

std::string join_command(const std::vector<std::string>& command)
{
    std::string out;
    bool first = true;
    for (const auto& part : command)
    {
        if (!first)
        {
            out.push_back(' ');
        }
        first = false;
        out += part;
    }
    return out;
}
}

Result<std::string> render_quadlet_container(const QuadletContainer& unit)
{
    if (unit.description.empty())
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "quadlet description is empty"));
    }
    if (auto result = validate_line_value("quadlet description", unit.description); !result)
    {
        return std::unexpected(result.error());
    }
    if (auto result = validate_container_spec(unit.container); !result)
    {
        return std::unexpected(result.error());
    }
    for (const auto& after : unit.after_units)
    {
        if (auto result = validate_line_value("After unit", after); !result)
        {
            return std::unexpected(result.error());
        }
    }
    if (auto result = validate_line_value("Restart", unit.restart); !result)
    {
        return std::unexpected(result.error());
    }

    std::ostringstream out;
    out << "[Unit]\n";
    out << "Description=" << unit.description << "\n";
    if (!unit.after_units.empty())
    {
        out << "After=";
        bool first = true;
        for (const auto& after : unit.after_units)
        {
            if (!first)
            {
                out << ' ';
            }
            first = false;
            out << after;
        }
        out << "\n";
    }

    out << "\n[Container]\n";
    out << "ContainerName=" << unit.container.name << "\n";
    out << "Image=" << unit.container.image << "\n";
    out << "ReadOnly=" << (unit.container.read_only_filesystem ? "true" : "false") << "\n";
    if (!unit.container.command.empty())
    {
        for (const auto& part : unit.container.command)
        {
            if (auto result = validate_line_value("Exec", part); !result)
            {
                return std::unexpected(result.error());
            }
        }
        out << "Exec=" << join_command(unit.container.command) << "\n";
    }
    for (const auto& [key, value] : unit.container.env)
    {
        if (auto result = validate_line_value("Environment", key + value); !result)
        {
            return std::unexpected(result.error());
        }
        out << "Environment=" << key << "=" << value << "\n";
    }
    for (const auto& [key, value] : unit.container.labels)
    {
        if (auto result = validate_line_value("Label", key + value); !result)
        {
            return std::unexpected(result.error());
        }
        out << "Label=" << key << "=" << value << "\n";
    }
    for (const auto& cap : unit.container.cap_drop)
    {
        if (auto result = validate_line_value("DropCapability", cap); !result)
        {
            return std::unexpected(result.error());
        }
        out << "DropCapability=" << cap << "\n";
    }
    for (const auto& option : unit.container.security_opt)
    {
        if (auto result = validate_line_value("SecurityLabelDisable", option); !result)
        {
            return std::unexpected(result.error());
        }
        out << "PodmanArgs=--security-opt=" << option << "\n";
    }

    out << "\n[Service]\n";
    out << "Restart=" << unit.restart << "\n";
    if (unit.service_policy)
    {
        if (auto result = validate_user_slice_policy(*unit.service_policy); !result)
        {
            return std::unexpected(result.error());
        }
        if (unit.service_policy->cpu_quota)
        {
            out << "CPUQuota=" << *unit.service_policy->cpu_quota << "\n";
        }
        if (unit.service_policy->cpu_weight)
        {
            out << "CPUWeight=" << *unit.service_policy->cpu_weight << "\n";
        }
        if (unit.service_policy->memory_max)
        {
            out << "MemoryMax=" << *unit.service_policy->memory_max << "\n";
        }
        if (unit.service_policy->tasks_max)
        {
            out << "TasksMax=" << *unit.service_policy->tasks_max << "\n";
        }
        if (unit.service_policy->allowed_cpus)
        {
            out << "AllowedCPUs=" << *unit.service_policy->allowed_cpus << "\n";
        }
    }

    out << "\n[Install]\n";
    out << "WantedBy=default.target\n";
    return out.str();
}
}
