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

- [Dockerfile.dev](/Users/sspingal/ws//vpp/Dockerfile.dev): development image
- [compose.dev.yaml](/Users/sspingal/ws//vpp/compose.dev.yaml): long-running dev container
- [vpp-dev](/Users/sspingal/ws//vpp/docker/dev/vpp-dev): helper wrapper around `docker compose`
- [startup.conf](/Users/sspingal/ws//vpp/docker/dev/startup.conf): default startup config for container runs
- [subscriber-dp-lab](/Users/sspingal/ws//vpp/docker/dev/subscriber-dp-lab): helper for wiring a traffic container to `vpp-dev`
- [vpp-ovs-lab](/Users/sspingal/ws//vpp/docker/dev/vpp-ovs-lab): helper for wiring two traffic containers through OVS and VPP for plain L3 forwarding

## First-time setup

Make sure your machine has either the Docker Compose plugin or the legacy
`docker-compose` binary installed.

### Ubuntu 22.04 quick start

On Ubuntu 22.04, a simple setup is:

```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose openvswitch-switch iproute2
sudo systemctl enable --now docker
sudo systemctl enable --now openvswitch-switch
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

If you are using legacy `docker-compose` 1.x on Ubuntu and hit an error like
`KeyError: 'ContainerConfig'` during `up`, that is a known recreate-path issue
in old Compose. The helper script works around it by removing the stale
`vpp-dev` container first before starting a new one.

From the VPP repo root:

```bash
cd /Users/sspingal/ws//vpp
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

- plugin code under `/Users/sspingal/ws//vpp/src/plugins/...`
- startup configs under `/Users/sspingal/ws//vpp/startup_configs/...`

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

## subscriber_dp traffic lab

To exercise the current `subscriber_dp` plugin with live traffic, use the lab
helper:

```bash
docker/dev/subscriber-dp-lab prepare
docker/dev/subscriber-dp-lab validate-ping
docker/dev/subscriber-dp-lab status
```

What it does:

- creates a `traffic` container with no Docker-managed data network
- creates a dedicated `veth` pair
- moves one end into `vpp-dev`
- moves the other end into `traffic`
- configures the VPP host-interface and a matching `subscriber_dp` entry
- validates with `ping`

Defaults:

- traffic container IP: `10.10.0.2/24`
- VPP host-interface IP: `10.10.0.1/24`
- subscriber ID: `1001`

Clean everything up with:

```bash
docker/dev/subscriber-dp-lab cleanup
```

## OVS L3 forwarding lab

To exercise plain VPP L3 forwarding without `subscriber_dp`, use the OVS lab
helper:

```bash
docker/dev/vpp-ovs-lab prepare
docker/dev/vpp-ovs-lab enable-subscriber-dp
docker/dev/vpp-ovs-lab validate-ping
docker/dev/vpp-ovs-lab validate-iperf tcp
docker/dev/vpp-ovs-lab validate-iperf tcp --parallel 4
docker/dev/vpp-ovs-lab validate-iperf udp --bandwidth 1G
docker/dev/vpp-ovs-lab benchmark
docker/dev/vpp-ovs-lab status
```

What it does:

- creates two OVS bridges, one per L3 segment
- creates a `traffic-client` container and a `traffic-server` container
- wires both traffic containers and the `vpp-dev` container into OVS using
  dedicated `veth` pairs
- creates `host-vpp-a` and `host-vpp-b` inside VPP and configures them as the
  default gateway on both subnets
- validates connectivity with `ping`
- runs `iperf3` from the client container to the server container through VPP

Topology:

```text
traffic-client 192.168.10.2/24 -- OVS bridge br-vpp-a -- vpp-dev host-vpp-a 192.168.10.1/24
traffic-server 192.168.20.2/24 -- OVS bridge br-vpp-b -- vpp-dev host-vpp-b 192.168.20.1/24
```

The client container uses `192.168.10.1` as its default gateway. The server
container uses `192.168.20.1` as its default gateway.

Routing explanation:

- OVS is only switching Ethernet frames inside each subnet
- VPP is routing between the two connected subnets
- we do not add static routes in VPP because `set interface ip address` creates
  connected routes automatically
- we do add default routes inside the traffic containers, pointing at the VPP
  interface IP on each subnet
- once both VPP interfaces are up with IP addresses, forwarding between
  `192.168.10.0/24` and `192.168.20.0/24` works without any extra route CLI

To enforce subscriber-aware admission, first start VPP with the plugin enabled
in your startup config, then run:

```bash
docker/dev/vpp-ovs-lab enable-subscriber-dp
```

That enables `subscriber_dp` on `host-vpp-a` and `host-vpp-b`, then adds the
default subscriber entries for:

- `192.168.10.2` on `host-vpp-a`
- `192.168.20.2` on `host-vpp-b`

With the new node behavior, packets miss-dropping is now the default once the
feature is enabled on an interface.

Manual VPP CLI equivalent:

```text
subscriber-dp enable-disable host-vpp-a ip4
subscriber-dp enable-disable host-vpp-b ip4

subscriber-dp subscriber add host-vpp-a address 192.168.10.2 id 1
subscriber-dp subscriber add host-vpp-b address 192.168.20.2 id 2
```

Those subscriber IPs are the ingress source addresses VPP actually sees on the
two interfaces. They are not the VPP gateway addresses.

Disable it again with:

```bash
docker/dev/vpp-ovs-lab disable-subscriber-dp
```

Useful `iperf3` variants:

- TCP single stream:
  `docker/dev/vpp-ovs-lab validate-iperf tcp`
- TCP reverse:
  `docker/dev/vpp-ovs-lab validate-iperf tcp --reverse`
- TCP four streams:
  `docker/dev/vpp-ovs-lab validate-iperf tcp --parallel 4`
- UDP with explicit rate:
  `docker/dev/vpp-ovs-lab validate-iperf udp --bandwidth 1G`
- Default benchmark suite:
  `docker/dev/vpp-ovs-lab benchmark`

Useful subscriber tests:

- positive case:
  enable `subscriber_dp`, keep both subscriber entries, then run
  `docker/dev/vpp-ovs-lab validate-ping` or `docker/dev/vpp-ovs-lab validate-iperf tcp`
- negative case:
  delete one subscriber entry, for example
  `subscriber-dp subscriber del host-vpp-b address 192.168.20.2`
  then retry ping or `iperf3` and watch `lookup-miss` / `lookup-drop` increase
  in `show subscriber-dp`

Useful observability commands in VPP:

```text
show subscriber-dp
show subscriber-dp interfaces
show subscriber-dp flows
show subscriber-dp services
show ip fib
show ip neighbor
```

Defaults:

- bridge A subnet: `192.168.10.0/24`
- bridge B subnet: `192.168.20.0/24`
- VPP gateway IPs: `192.168.10.1/24` and `192.168.20.1/24`
- client IP: `192.168.10.2/24`
- server IP: `192.168.20.2/24`

This lab intentionally uses OVS only as an L2 switch. VPP does the L3
forwarding.

Clean everything up with:

```bash
docker/dev/vpp-ovs-lab cleanup
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
- The default [startup.conf](/Users/sspingal/ws//vpp/docker/dev/startup.conf)
  is only a minimal dev convenience config. Real dataplane experiments should
  use a dedicated startup file under `/Users/sspingal/ws//vpp/startup_configs`.
