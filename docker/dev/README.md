# VPP Docker Dev Workflow

This directory documents the container-based VPP development workflow used in
this repo.

The setup is meant for daily iteration:

- source code is edited on the host and bind-mounted into the container
- build outputs are kept in `/cache/vpp-build-root` inside a Docker volume
- `ccache` is kept in a Docker volume to speed up rebuilds
- VPP can be started with either the default container config or a custom
  startup config from the repo

The helper script supports both:

- `docker compose`
- `docker-compose`

## What gets created

- [Dockerfile.dev](/Users/sspingal/ws/yuvo/vpp/Dockerfile.dev): development image
- [compose.dev.yaml](/Users/sspingal/ws/yuvo/vpp/compose.dev.yaml): long-running dev container
- [vpp-dev](/Users/sspingal/ws/yuvo/vpp/docker/dev/vpp-dev): helper wrapper around `docker compose`
- [startup.conf](/Users/sspingal/ws/yuvo/vpp/docker/dev/startup.conf): default startup config for container runs

## First-time setup

Make sure your machine has either the Docker Compose plugin or the legacy
`docker-compose` binary installed.

### Ubuntu 22.04 quick start

On Ubuntu 22.04, a simple setup is:

```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

After adding your user to the `docker` group, log out and log back in before
running Docker commands without `sudo`.

Depending on your installation, either of these may be available:

```bash
docker compose version
docker-compose version
```

The helper script supports both forms.

From the VPP repo root:

```bash
cd /Users/sspingal/ws/yuvo/vpp
docker/dev/vpp-dev up
```

This builds the dev image, starts the `vpp-dev` container, and creates the
persistent Docker volumes used for build outputs and compiler cache.

If the dev container setup changes later, recreate it:

```bash
docker compose -f compose.dev.yaml down
docker/dev/vpp-dev up
```

## First build

Build the release tree:

```bash
docker/dev/vpp-dev build-release
```

Build outputs are stored under:

```text
/cache/vpp-build-root
```

inside the container. The corresponding installed VPP binary is typically:

```text
/cache/vpp-build-root/install-vpp-native/vpp/bin/vpp
```

## Running VPP

Run VPP with the default container startup config:

```bash
docker/dev/vpp-dev run
```

Run VPP with your own startup config:

```bash
docker/dev/vpp-dev run /workspace/vpp/startup_configs/no_dpdk2.conf
```

If your startup config is a host path under the repo, the equivalent path
inside the container is `/workspace/vpp/...`.

Run the debug build instead of the release build:

```bash
docker/dev/vpp-dev run-debug /workspace/vpp/startup_configs/no_dpdk2.conf
```

## Day-to-day workflow

### Edit code

Edit files on the host normally, for example:

- plugin code under `/Users/sspingal/ws/yuvo/vpp/src/plugins/...`
- startup configs under `/Users/sspingal/ws/yuvo/vpp/startup_configs/...`

Because the repo is bind-mounted, the container sees those changes immediately.

### Rebuild after code changes

For the normal release build:

```bash
docker/dev/vpp-dev build-release
```

For the debug build:

```bash
docker/dev/vpp-dev build
```

This is incremental. The build cache is preserved in Docker volumes, so
rebuilding after small source changes is much faster than a clean build.

### Run tests

Run a specific VPP test:

```bash
docker/dev/vpp-dev test TEST=xxx
```

Examples:

```bash
docker/dev/vpp-dev test TEST=test_ping.py
docker/dev/vpp-dev test TEST=some_suite.SomeTestCase
```

### Open a shell in the container

```bash
docker/dev/vpp-dev shell
```

Useful when you want to inspect build outputs or run `vppctl` manually.

## Typical edit-build-run loop

1. Change code on the host.
2. Rebuild:

```bash
docker/dev/vpp-dev build-release
```

3. Start VPP:

```bash
docker/dev/vpp-dev run /workspace/vpp/startup_configs/no_dpdk2.conf
```

4. In another terminal, inspect VPP:

```bash
docker/dev/vpp-dev shell
```

Then inside the container:

```bash
/cache/vpp-build-root/install-vpp-native/vpp/bin/vppctl -s /run/vpp/cli.sock show version
/cache/vpp-build-root/install-vpp-native/vpp/bin/vppctl -s /run/vpp/cli.sock show plugins
```

## Startup config guidance

For container-only runs without NIC/PMD setup, disable DPDK explicitly in your
startup config:

```conf
plugins {
    plugin default { enable }
    plugin dpdk_plugin.so { disable }
}
```

For plugins that are compiled with `.default_disabled = 1`, add an explicit
enable line. Example:

```conf
plugins {
    plugin default { enable }
    plugin dpdk_plugin.so { disable }
    plugin subscriber_dp_plugin.so { enable }
}
```

## Known behavior in this container

- Hugepage warnings are expected unless you explicitly configure buffer pages
  to use normal pages.
- `perfmon` warnings are expected in environments where perf counters are not
  available.
- Some tests depend on Linux kernel behavior and may differ on Docker Desktop
  versus native Linux.

## Notes

- `make run` already supports `STARTUP_CONF=<path>`. The helper script just
  passes it through.
- The repo's checked-in `build-root` stays visible; only generated outputs are
  redirected to `/cache/vpp-build-root` through `MAKEFLAGS=BR=/cache/vpp-build-root`.
- The default [startup.conf](/Users/sspingal/ws/yuvo/vpp/docker/dev/startup.conf)
  is only a minimal dev convenience config. Real dataplane experiments should
  use a dedicated startup file under `/Users/sspingal/ws/yuvo/vpp/startup_configs`.
