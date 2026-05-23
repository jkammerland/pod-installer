# podman-manager

MVP C++23 library and example service for a root-owned orchestrator that manages
containers for multiple rootless Podman users. The library talks to
`podman system service` over per-user Unix sockets and keeps systemd/Quadlet
integration behind small adapters.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Required build dependency: libcurl. The example service intentionally uses a
small local HTTP listener instead of `uWebSockets` because `uWebSockets` is not
installed in this environment. The orchestration handler is separate from the
listener so a `uWebSockets` adapter can replace it later.

## Library Surface

- `PodmanClient`: HTTP over AF_UNIX using libcurl, with helpers for `_ping`,
  info, list, create, start, stop, remove, and image archive load.
- `ContainerSpec`: minimal SpecGenerator-compatible JSON for one-container
  deployments with labels, env, read-only filesystem, dropped capabilities,
  security options, and CPU/memory/pids limits.
- `validate_podman_socket`: rejects symlinks, non-sockets, wrong owners, and
  paths outside the expected `/run/user/<uid>/podman/podman.sock` layout.
- `SystemctlSliceController`: applies runtime-only `user-<uid>.slice`
  properties via `posix_spawnp("systemctl", ...)`, avoiding shell
  interpolation.
- `render_quadlet_container`: renders a minimal `.container` Quadlet file for
  deployments that should be handed to systemd/Podman instead of REST calls.

## Example Service

Dry-run mode is the default and does not contact Podman:

```bash
./build/podman_manager_example_service --listen 127.0.0.1:9090
curl -X POST \
  'http://127.0.0.1:9090/v1/deploy?user=alice&name=demo&image=busybox&cmd=/bin/sh&arg=-lc&arg=sleep%2060'
```

Execute mode validates the target rootless socket, pings Podman, creates the
container, and starts it:

```bash
sudo ./build/podman_manager_example_service --execute --apply-systemd
curl -X POST \
  'http://127.0.0.1:9090/v1/deploy?user=alice&name=demo&image=busybox&cmd=/bin/sh&arg=-lc&arg=sleep%2060&cpuQuota=200%25&memoryMax=1G&tasksMax=512&allowedCpus=2-3'
```

This example is local-only and unauthenticated. It is meant to demonstrate the
library boundary, not to be exposed as a production API.
