# podman-manager

MVP C++23 library and example service for a root-owned orchestrator that deploys
supplied image archives and supplied Quadlet files for multiple rootless Podman
users.

The current deployment path is:

1. validate the supplied bundle and Quadlet policy,
2. optionally load an image archive through the target user's Podman API socket,
3. atomically install the supplied `.container` Quadlet under
   `/etc/containers/systemd/users/<uid>/`,
4. reload the target user's systemd manager,
5. restart the generated `.service` unit.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Required build dependency: libcurl. If `sdbus-c++` is available, CMake also
builds the optional D-Bus systemd backend. The example service intentionally
uses a small local HTTP listener instead of `uWebSockets` because `uWebSockets`
is not installed in this environment. The orchestration handler is separate from
the listener so a `uWebSockets` adapter can replace it later.

## Library Surface

- `PodmanClient`: HTTP over AF_UNIX using libcurl, with helpers for `_ping`,
  info, list, create, start, stop, remove, and image archive load.
- `DeploymentOrchestrator`: deploys a supplied image archive and supplied
  `.container` Quadlet for a target uid.
- `QuadletInstaller`: validates policy and atomically installs supplied Quadlet
  text into the configured admin-controlled rootless search path.
- `UserSystemdController`: lifecycle abstraction for user systemd managers,
  with a `systemctl --user --machine=<user>@.host` backend and an optional
  `sdbus-c++` backend.
- `ContainerSpec`: minimal SpecGenerator-compatible JSON for one-container
  deployments. This remains useful for direct Podman REST flows, but the
  bundle deployment path does not generate Quadlets from it.
- `validate_podman_socket`: rejects symlinks, non-sockets, wrong owners, and
  paths outside the expected `/run/user/<uid>/podman/podman.sock` layout.
- `SystemctlSliceController`: applies runtime-only `user-<uid>.slice`
  properties via `posix_spawnp("systemctl", ...)`, avoiding shell
  interpolation.

## Example Service

Stage an image archive and Quadlet file. Dry-run mode is the default and does
not contact Podman, write the Quadlet, or call systemd:

```bash
./build/podman_manager_example_service --listen 127.0.0.1:9090

curl -X POST \
  'http://127.0.0.1:9090/v1/deploy-bundle?user=alice&quadletPath=/var/lib/podman-manager/staging/demo.container&imageArchive=/var/lib/podman-manager/staging/demo.oci.tar&revision=42'
```

Execute mode validates the target rootless socket when loading an image archive,
loads the image, installs the Quadlet, reloads the user manager, and restarts
the generated service:

```bash
sudo ./build/podman_manager_example_service --execute

curl -X POST \
  'http://127.0.0.1:9090/v1/deploy-bundle?user=alice&quadletPath=/var/lib/podman-manager/staging/demo.container&imageArchive=/var/lib/podman-manager/staging/demo.oci.tar&revision=42'
```

This example is local-only and unauthenticated. It is meant to demonstrate the
library boundary, not to be exposed as a production API.

## Supplied Quadlet Example

```ini
[Unit]
Description=Demo service

[Container]
Image=localhost/demo:latest
Label=com.example.podman-manager.managed=true
ReadOnly=true
NoNewPrivileges=true
DropCapability=all

[Service]
Restart=on-failure
TimeoutStartSec=900

[Install]
WantedBy=default.target
```

The MVP policy rejects privileged containers, host networking, host PID/IPC/user
namespaces, devices, root filesystem bind mounts, denied `PodmanArgs`, missing
`Image=`, and missing managed label.
