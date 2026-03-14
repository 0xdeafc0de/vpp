# subscriber_dp Plugin

This document describes the current state of the `subscriber_dp` VPP plugin.

The plugin is an early scaffold for a subscriber-aware dataplane. At this stage
it provides:

- a main-thread-owned subscriber table
- worker-thread read lookups from the packet path
- binary APIs for enable, add, update, delete, and dump
- VPP CLI commands for local testing

It does not yet provide:

- per-worker established-flow tables
- subscriber-to-worker pinning
- LB plugin integration
- CE integration
- policy enforcement actions

## Current behavior

The plugin stores subscriber entries keyed by:

- `sw_if_index`
- address family
- exact IP address

Each entry contains:

- IP address
- interface index
- `subscriber_id`
- IP family flag

The lookup node runs on:

- `ip4-unicast`
- `ip6-unicast`

For now, the node uses the source IP address of the packet for lookup and
stores the result in buffer metadata (`opaque2`) as:

- `subscriber_valid`
- `subscriber_id`

## Build and load

The plugin source lives in:

- [subscriber_dp.c](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/subscriber_dp.c)
- [subscriber_dp_node.c](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/subscriber_dp_node.c)
- [subscriber_dp_api.c](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/subscriber_dp_api.c)
- [subscriber_dp.api](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/subscriber_dp.api)

Build it with the normal VPP build:

```bash
cd /Users/sspingal/ws//vpp
docker/dev/vpp-dev build-release
```

The installed plugin file is:

```text
subscriber_dp_plugin.so
```

Important: this plugin is marked `default_disabled = 1` in
[plugin.c](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/plugin.c), so
you must explicitly enable it in `startup.conf`:

```conf
plugins {
    plugin default { enable }
    plugin dpdk_plugin.so { disable }
    plugin subscriber_dp_plugin.so { enable }
}
```

## CLI commands

The plugin currently exposes these VPP CLI commands:

### Enable or disable the feature

Enable on both IPv4 and IPv6 unicast arcs:

```text
subscriber-dp enable-disable <interface>
```

Disable:

```text
subscriber-dp enable-disable <interface> disable
```

Enable on only one family:

```text
subscriber-dp enable-disable <interface> ip4
subscriber-dp enable-disable <interface> ip6
```

### Add, update, and delete subscriber entries

Add:

```text
subscriber-dp subscriber add <interface> address <ip> id <subscriber-id>
```

Update:

```text
subscriber-dp subscriber update <interface> address <ip> id <subscriber-id>
```

Delete:

```text
subscriber-dp subscriber del <interface> address <ip>
```

Examples:

```text
subscriber-dp subscriber add local0 address 10.1.1.10 id 1001
subscriber-dp subscriber update local0 address 10.1.1.10 id 2002
subscriber-dp subscriber del local0 address 10.1.1.10
```

IPv6 works as well:

```text
subscriber-dp subscriber add local0 address 2001:db8::10 id 3001
```

### Show current state

```text
show subscriber-dp
show subscriber-dp interfaces
```

This displays:

- add/update/delete counters
- lookup hit/miss counters
- current subscriber entries
- optionally, interfaces where the feature is enabled

### Trace support

The packet-path nodes support per-node tracing. Example:

```text
trace add subscriber-dp-ip4 20
show trace
```

## Binary APIs

The plugin also exposes binary APIs defined in
[subscriber_dp.api](/Users/sspingal/ws//vpp/src/plugins/subscriber_dp/subscriber_dp.api):

- `subscriber_dp_enable_disable`
- `subscriber_dp_subscriber_add`
- `subscriber_dp_subscriber_del`
- `subscriber_dp_subscriber_update`
- `subscriber_dp_subscriber_dump`

These are the APIs intended for sidecar or external control-plane programming.

## Current lookup model

The subscriber table is owned by the main thread.

Writes use worker barriers:

- add
- update
- delete

Workers perform read-only exact-match lookups during packet processing.

This is deliberate for the current phase because it gives us:

- a control-plane-programmable subscriber map
- worker-side access without inter-worker messaging for reads

It does not yet guarantee that all traffic for a subscriber lands on the same
worker.

## Limitations and next steps

Current limitations:

- lookup key is exact IP address only
- no prefix, range, or tuple matching
- no flow table
- no subscriber affinity to a worker
- no CE handoff or CE context integration
- no subscriber-aware reverse-direction correlation

Likely next steps:

1. Add a per-worker flow table.
2. Attach subscriber metadata to flow state.
3. Define the sidecar control-plane model over binary API.
4. Add CE integration inside the worker packet path.
5. Revisit worker affinity and handoff strategy.
