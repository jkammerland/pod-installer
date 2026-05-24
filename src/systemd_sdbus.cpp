#include "podman_manager/systemd.hpp"

#if PODMAN_MANAGER_HAS_SDBUS

#include <sdbus-c++/sdbus-c++.h>

#include <memory>

namespace podman_manager
{
namespace
{
constexpr auto systemd_service = "org.freedesktop.systemd1";
constexpr auto systemd_path = "/org/freedesktop/systemd1";
constexpr auto manager_interface = "org.freedesktop.systemd1.Manager";
constexpr auto unit_interface = "org.freedesktop.systemd1.Unit";
constexpr auto properties_interface = "org.freedesktop.DBus.Properties";

std::string user_bus_address(uid_t uid)
{
    return "unix:path=/run/user/" + std::to_string(uid) + "/bus";
}

std::unique_ptr<sdbus::IProxy> manager_proxy(const PodmanTarget& target)
{
    auto connection = sdbus::createSessionBusConnectionWithAddress(user_bus_address(target.uid));
    return sdbus::createProxy(std::move(connection),
                              sdbus::ServiceName{systemd_service},
                              sdbus::ObjectPath{systemd_path},
                              sdbus::dont_run_event_loop_thread);
}

Result<std::string> unit_job(const PodmanTarget& target,
                             std::string_view method,
                             std::string_view unit)
{
    if (auto result = validate_systemd_unit_name(unit); !result)
    {
        return std::unexpected(result.error());
    }

    try
    {
        auto proxy = manager_proxy(target);
        sdbus::ObjectPath job;
        proxy->callMethod(std::string{method})
            .onInterface(manager_interface)
            .withArguments(std::string{unit}, std::string{"replace"})
            .storeResultsTo(job);
        return std::string{job};
    }
    catch (const sdbus::Error& error)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          std::string{"systemd D-Bus "} + std::string{method} +
                                              " failed: " + error.getName() + ": " +
                                              error.getMessage()));
    }
}

Result<std::string> get_unit_property_string(sdbus::IProxy& proxy,
                                             std::string_view property)
{
    try
    {
        sdbus::Variant value;
        proxy.callMethod("Get")
            .onInterface(properties_interface)
            .withArguments(std::string{unit_interface}, std::string{property})
            .storeResultsTo(value);
        return value.get<std::string>();
    }
    catch (const sdbus::Error& error)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "systemd D-Bus property read failed: " +
                                              error.getName() + ": " + error.getMessage()));
    }
}
}

Result<void> SdbusUserSystemdController::daemon_reload(const PodmanTarget& target) const
{
    try
    {
        auto proxy = manager_proxy(target);
        proxy->callMethod("Reload").onInterface(manager_interface);
        return {};
    }
    catch (const sdbus::Error& error)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "systemd D-Bus Reload failed: " +
                                              error.getName() + ": " + error.getMessage()));
    }
}

Result<std::string> SdbusUserSystemdController::start_unit(const PodmanTarget& target,
                                                           std::string_view unit) const
{
    return unit_job(target, "StartUnit", unit);
}

Result<std::string> SdbusUserSystemdController::restart_unit(const PodmanTarget& target,
                                                             std::string_view unit) const
{
    return unit_job(target, "RestartUnit", unit);
}

Result<std::string> SdbusUserSystemdController::stop_unit(const PodmanTarget& target,
                                                          std::string_view unit) const
{
    return unit_job(target, "StopUnit", unit);
}

Result<UnitStatus> SdbusUserSystemdController::status(const PodmanTarget& target,
                                                      std::string_view unit) const
{
    if (auto result = validate_systemd_unit_name(unit); !result)
    {
        return std::unexpected(result.error());
    }

    try
    {
        auto manager = manager_proxy(target);
        sdbus::ObjectPath unit_path;
        manager->callMethod("GetUnit")
            .onInterface(manager_interface)
            .withArguments(std::string{unit})
            .storeResultsTo(unit_path);

        auto connection = sdbus::createSessionBusConnectionWithAddress(user_bus_address(target.uid));
        auto unit_proxy = sdbus::createProxy(std::move(connection),
                                             sdbus::ServiceName{systemd_service},
                                             unit_path,
                                             sdbus::dont_run_event_loop_thread);

        UnitStatus out;
        out.unit = std::string{unit};

        auto load = get_unit_property_string(*unit_proxy, "LoadState");
        if (!load)
        {
            return std::unexpected(load.error());
        }
        auto active = get_unit_property_string(*unit_proxy, "ActiveState");
        if (!active)
        {
            return std::unexpected(active.error());
        }
        auto sub = get_unit_property_string(*unit_proxy, "SubState");
        if (!sub)
        {
            return std::unexpected(sub.error());
        }

        out.load_state = *load;
        out.active_state = *active;
        out.sub_state = *sub;
        return out;
    }
    catch (const sdbus::Error& error)
    {
        return std::unexpected(make_error(ErrorKind::systemd,
                                          "systemd D-Bus status failed: " +
                                              error.getName() + ": " + error.getMessage()));
    }
}
}

#endif

