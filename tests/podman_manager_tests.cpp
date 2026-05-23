#include "podman_manager/podman_manager.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pm = podman_manager;

namespace
{
class TempDir
{
public:
    TempDir()
    {
        auto pattern = std::filesystem::temp_directory_path() / "podman-manager-test-XXXXXX";
        auto value = pattern.string();
        char* created = mkdtemp(value.data());
        assert(created != nullptr);
        path_ = created;
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct UnixListener
{
    int fd{-1};
    std::filesystem::path path;

    explicit UnixListener(std::filesystem::path socket_path)
        : path{std::move(socket_path)}
    {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(fd >= 0);

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const auto path_string = path.string();
        assert(path_string.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, path_string.c_str(), sizeof(addr.sun_path) - 1);

        std::filesystem::create_directories(path.parent_path());
        unlink(path_string.c_str());
        assert(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        assert(listen(fd, 8) == 0);
    }

    ~UnixListener()
    {
        if (fd >= 0)
        {
            close(fd);
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::string read_http_request(int client)
{
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos)
    {
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        assert(n > 0);
        request.append(buffer, static_cast<size_t>(n));
    }

    const auto content_length_key = std::string{"Content-Length:"};
    const auto header_end = request.find("\r\n\r\n");
    const auto length_pos = request.find(content_length_key);
    if (length_pos != std::string::npos)
    {
        const auto line_end = request.find("\r\n", length_pos);
        auto raw_length = request.substr(length_pos + content_length_key.size(),
                                         line_end - (length_pos + content_length_key.size()));
        const auto length = static_cast<size_t>(std::stoul(raw_length));
        const auto have = request.size() - (header_end + 4);
        while (have < length && request.size() < header_end + 4 + length)
        {
            const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            assert(n > 0);
            request.append(buffer, static_cast<size_t>(n));
        }
    }

    return request;
}

void send_response(int client, int status, std::string body = "OK", std::string extra_headers = {})
{
    const char* reason = status == 200 ? "OK" : status == 201 ? "Created" : "No Content";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    out << extra_headers;
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    const auto response = out.str();
    assert(send(client, response.data(), response.size(), MSG_NOSIGNAL) > 0);
}

void test_container_spec_json()
{
    pm::ContainerSpec spec;
    spec.name = "svc-u1000-demo-r42";
    spec.image = "localhost/acme/demo:1.2.3";
    spec.command = {"/bin/sh", "-lc", "echo \"hello\" && sleep 1"};
    spec.env = {{"ACME_MODE", "production\nnext"}};
    spec.labels = {{"com.example.managed", "true"}};
    spec.resource_limits = pm::ResourceLimits{
        .cpu = pm::CpuLimits{.period = 100000, .quota = 50000, .shares = 128, .cpus = "2-3"},
        .memory = pm::MemoryLimits{.limit = 268435456, .swap = 268435456},
        .pids = pm::PidsLimits{.limit = 128},
    };

    auto json = pm::to_podman_create_json(spec);
    assert(json);
    assert(json->find(R"("name":"svc-u1000-demo-r42")") != std::string::npos);
    assert(json->find(R"("command":["/bin/sh","-lc","echo \"hello\" && sleep 1"])") != std::string::npos);
    assert(json->find(R"("ACME_MODE":"production\nnext")") != std::string::npos);
    assert(json->find(R"("cpus":"2-3")") != std::string::npos);
}

void test_container_spec_validation_edges()
{
    pm::ContainerSpec invalid_name;
    invalid_name.name = "../bad";
    invalid_name.image = "busybox";
    assert(!pm::to_podman_create_json(invalid_name));

    pm::ContainerSpec invalid_cpu;
    invalid_cpu.name = "ok";
    invalid_cpu.image = "busybox";
    pm::CpuLimits cpu;
    cpu.quota = -1;
    pm::ResourceLimits limits;
    limits.cpu = cpu;
    invalid_cpu.resource_limits = limits;
    auto result = pm::to_podman_create_json(invalid_cpu);
    assert(!result);
    assert(result.error().kind == pm::ErrorKind::policy);
}

void test_url_codec()
{
    assert(pm::url_encode_component("a b/c") == "a%20b%2Fc");
    auto decoded = pm::url_decode_component("a+b%2Fc", true);
    assert(decoded);
    assert(*decoded == "a b/c");
    assert(!pm::url_decode_component("%X0"));
}

void test_socket_validation()
{
    TempDir temp;
    UnixListener listener{temp.path() / "podman.sock"};
    pm::PodmanTarget target{.uid = getuid(),
                            .user_name = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};

    pm::SocketValidationOptions options;
    options.require_default_path = false;
    assert(pm::validate_podman_socket(target, options));

    const auto symlink_path = temp.path() / "link.sock";
    std::filesystem::create_symlink(listener.path.filename(), symlink_path);
    target.socket_path = symlink_path;
    assert(!pm::validate_podman_socket(target, options));
}

void test_systemd_args_and_quadlet()
{
    pm::UserSlicePolicy policy{.uid = 1000,
                               .cpu_quota = "200%",
                               .cpu_weight = 100,
                               .memory_max = "1G",
                               .tasks_max = 512,
                               .allowed_cpus = "2-3"};
    auto args = pm::build_systemctl_set_property_args(policy);
    assert(args);
    assert((*args)[0] == "systemctl");
    assert((*args)[2] == "--runtime");
    assert((*args)[3] == "user-1000.slice");
    assert(std::ranges::find(*args, "AllowedCPUs=2-3") != args->end());

    pm::ContainerSpec spec;
    spec.name = "demo";
    spec.image = "localhost/demo:latest";
    spec.command = {"/usr/bin/demo"};
    spec.labels = {{"com.example.managed", "true"}};
    auto quadlet = pm::render_quadlet_container(pm::QuadletContainer{.description = "Demo container",
                                                                     .container = spec,
                                                                     .service_policy = policy});
    assert(quadlet);
    assert(quadlet->find("[Container]\n") != std::string::npos);
    assert(quadlet->find("ContainerName=demo\n") != std::string::npos);
    assert(quadlet->find("CPUQuota=200%\n") != std::string::npos);

    spec.env = {{"BAD", "line\nbreak"}};
    pm::QuadletContainer bad_unit;
    bad_unit.description = "Bad";
    bad_unit.container = spec;
    auto bad_quadlet = pm::render_quadlet_container(bad_unit);
    assert(!bad_quadlet);

    spec.env.clear();
    spec.command = {"/usr/bin/demo", "line\nbreak"};
    bad_unit.container = spec;
    assert(!pm::render_quadlet_container(bad_unit));
}

void test_podman_client_against_fake_unix_server()
{
    TempDir temp;
    UnixListener listener{temp.path() / "podman.sock"};

    std::promise<void> ready;
    std::vector<std::string> requests;
    std::jthread server{[&](std::stop_token) {
        ready.set_value();
        for (int i = 0; i < 3; ++i)
        {
            const int client = accept(listener.fd, nullptr, nullptr);
            assert(client >= 0);
            auto request = read_http_request(client);
            requests.push_back(request);
            if (request.starts_with("GET /_ping "))
            {
                send_response(client, 200, "OK", "Libpod-API-Version: 5.0.0\r\n");
            }
            else if (request.starts_with("POST /v5.0.0/libpod/containers/create "))
            {
                send_response(client, 201, R"({"Id":"abc"})");
            }
            else if (request.starts_with("POST /v5.0.0/libpod/containers/demo/start "))
            {
                send_response(client, 204, "");
            }
            else
            {
                send_response(client, 500, request);
            }
            close(client);
        }
    }};
    ready.get_future().wait();

    pm::PodmanTarget target{.uid = getuid(),
                            .user_name = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};
    pm::PodmanClient client{target, pm::ClientOptions{.timeout = std::chrono::milliseconds{2000}}};
    assert(client.ping());

    pm::ContainerSpec spec;
    spec.name = "demo";
    spec.image = "busybox";
    assert(client.create_container(spec));
    assert(client.start_container("demo"));

    server.join();
    assert(requests.size() == 3);
    assert(requests[1].find(R"("image":"busybox")") != std::string::npos);
}
}

int main()
{
    test_container_spec_json();
    test_container_spec_validation_edges();
    test_url_codec();
    test_socket_validation();
    test_systemd_args_and_quadlet();
    test_podman_client_against_fake_unix_server();

    std::cout << "podman_manager_tests passed\n";
    return 0;
}
