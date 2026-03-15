#include <vlib/vlib.h>
#include <vlib/threads.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <vnet/tcp/tcp_packet.h>
#include <vnet/udp/udp_packet.h>

#include "subscriber_dp.h"

subscriber_dp_main_t subscriber_dp_main;

typedef enum
{
  SUBSCRIBER_DP_CLI_ADD,
  SUBSCRIBER_DP_CLI_UPDATE,
  SUBSCRIBER_DP_CLI_DEL,
} subscriber_dp_cli_op_t;

static u8 *
format_subscriber_dp_entry (u8 *s, va_list *args)
{
  subscriber_dp_entry_t *entry = va_arg (*args, subscriber_dp_entry_t *);
  vnet_main_t *vnm = vnet_get_main ();

  s = format (s, "interface %U address ",
              format_vnet_sw_if_index_name, vnm, entry->sw_if_index);

  if (entry->is_ip6)
    s = format (s, "%U", format_ip6_address, &entry->address.ip6);
  else
    s = format (s, "%U", format_ip4_address, &entry->address.ip4);

  s = format (s, " subscriber-id %llu",
              (unsigned long long) entry->subscriber_id);
  return s;
}

static void
subscriber_dp_record_feature_state (subscriber_dp_main_t *sm, u32 sw_if_index,
                                    bool is_ip6, bool enable)
{
  if (is_ip6)
    sm->feature_enabled_ip6_by_sw_if =
      clib_bitmap_set (sm->feature_enabled_ip6_by_sw_if, sw_if_index, enable);
  else
    sm->feature_enabled_ip4_by_sw_if =
      clib_bitmap_set (sm->feature_enabled_ip4_by_sw_if, sw_if_index, enable);
}

static clib_error_t *
subscriber_dp_init (vlib_main_t *vm)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;

  sm->vlib_main = vm;
  sm->vnet_main = vnet_get_main ();

  clib_bihash_init_24_8 (&sm->subscriber_by_key, "subscriber-dp-subscriber-by-key",
                         1024 /* buckets */, 1 << 20 /* memory */);
  clib_bihash_init_48_8 (&sm->flow_by_key, "subscriber-dp-flow-by-key",
                         1024 /* buckets */, 1 << 22 /* memory */);

  sm->subscriber_sync_interval_sec = 10.0;
  sm->flow_age_interval_sec = 10.0;
  sm->stats_export_interval_sec = 10.0;
  sm->service_interval_sec = 10.0;
  sm->flow_timeout_sec = 60.0;
  sm->subscriber_sync_max_per_tick = 1024;
  sm->flow_age_max_remove_per_tick = 1024;
  sm->stats_socket_path =
    format (0, "/run/vpp/subscriber_dp_stats.sock%c", 0);

  return 0;
}

VLIB_INIT_FUNCTION (subscriber_dp_init);

int
subscriber_dp_enable_disable (u32 sw_if_index, bool is_ip6, bool enable)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;

  if (pool_is_free_index (sm->vnet_main->interface_main.sw_interfaces,
                          sw_if_index))
    return VNET_API_ERROR_INVALID_SW_IF_INDEX;

  int rv =
    vnet_feature_enable_disable (is_ip6 ? "ip6-unicast" : "ip4-unicast",
                                 is_ip6 ? "subscriber-dp-ip6" :
                                          "subscriber-dp-ip4",
                                 sw_if_index, enable, 0, 0);

  if (rv == 0)
    subscriber_dp_record_feature_state (sm, sw_if_index, is_ip6, enable);

  return rv;
}

static int
subscriber_dp_upsert (subscriber_dp_main_t *sm, u32 sw_if_index,
                      const ip46_address_t *address, bool is_ip6,
                      u64 subscriber_id, bool is_update)
{
  clib_bihash_kv_24_8_t kv;
  clib_bihash_kv_24_8_t result;
  subscriber_dp_entry_t *entry;
  uword index;

  subscriber_dp_make_kv (&kv, sw_if_index, address, is_ip6);

  vlib_worker_thread_barrier_sync (sm->vlib_main);

  if (clib_bihash_search_24_8 (&sm->subscriber_by_key, &kv, &result) == 0)
    {
      entry = pool_elt_at_index (sm->entries, result.value);
      if (!is_update)
        {
          vlib_worker_thread_barrier_release (sm->vlib_main);
          return VNET_API_ERROR_VALUE_EXIST;
        }

      entry->subscriber_id = subscriber_id;
      sm->update_ops++;
      vlib_worker_thread_barrier_release (sm->vlib_main);
      return 0;
    }

  if (is_update)
    {
      vlib_worker_thread_barrier_release (sm->vlib_main);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  pool_get_zero (sm->entries, entry);
  index = entry - sm->entries;

  entry->address = *address;
  entry->sw_if_index = sw_if_index;
  entry->subscriber_id = subscriber_id;
  entry->is_ip6 = is_ip6;

  kv.value = index;
  clib_bihash_add_del_24_8 (&sm->subscriber_by_key, &kv, 1 /* is_add */);
  sm->add_ops++;

  vlib_worker_thread_barrier_release (sm->vlib_main);
  return 0;
}

int
subscriber_dp_entry_add (subscriber_dp_main_t *sm, u32 sw_if_index,
                         const ip46_address_t *address, bool is_ip6,
                         u64 subscriber_id)
{
  return subscriber_dp_upsert (sm, sw_if_index, address, is_ip6,
                               subscriber_id, false /* is_update */);
}

int
subscriber_dp_entry_update (subscriber_dp_main_t *sm, u32 sw_if_index,
                            const ip46_address_t *address, bool is_ip6,
                            u64 subscriber_id)
{
  return subscriber_dp_upsert (sm, sw_if_index, address, is_ip6,
                               subscriber_id, true /* is_update */);
}

int
subscriber_dp_entry_del (subscriber_dp_main_t *sm, u32 sw_if_index,
                         const ip46_address_t *address, bool is_ip6)
{
  clib_bihash_kv_24_8_t kv;
  clib_bihash_kv_24_8_t result;
  subscriber_dp_entry_t *entry;

  subscriber_dp_make_kv (&kv, sw_if_index, address, is_ip6);

  vlib_worker_thread_barrier_sync (sm->vlib_main);

  if (clib_bihash_search_24_8 (&sm->subscriber_by_key, &kv, &result) != 0)
    {
      vlib_worker_thread_barrier_release (sm->vlib_main);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  entry = pool_elt_at_index (sm->entries, result.value);
  pool_put (sm->entries, entry);
  clib_bihash_add_del_24_8 (&sm->subscriber_by_key, &kv, 0 /* is_add */);
  sm->del_ops++;

  vlib_worker_thread_barrier_release (sm->vlib_main);
  return 0;
}

subscriber_dp_entry_t *
subscriber_dp_lookup (subscriber_dp_main_t *sm, u32 sw_if_index,
                      const ip46_address_t *address, bool is_ip6)
{
  clib_bihash_kv_24_8_t kv;
  clib_bihash_kv_24_8_t result;

  subscriber_dp_make_kv (&kv, sw_if_index, address, is_ip6);

  if (clib_bihash_search_24_8 (&sm->subscriber_by_key, &kv, &result) != 0)
    {
      sm->lookup_misses++;
      return 0;
    }

  sm->lookup_hits++;
  return pool_elt_at_index (sm->entries, result.value);
}

typedef struct
{
  ip46_address_t src_address;
  ip46_address_t dst_address;
  u8 protocol;
  u16 src_port;
  u16 dst_port;
} subscriber_dp_flow_key_parts_t;

static bool
subscriber_dp_get_flow_parts (vlib_main_t *vm, vlib_buffer_t *b, bool is_ip6,
                              subscriber_dp_flow_key_parts_t *parts)
{
  u8 *current = vlib_buffer_get_current (b);
  u32 packet_len = vlib_buffer_length_in_chain (vm, b);
  u32 l4_offset = 0;

  clib_memset (parts, 0, sizeof (*parts));

  if (is_ip6)
    {
      ip6_header_t *ip6 = (ip6_header_t *) current;
      if (packet_len < sizeof (*ip6))
        return false;

      parts->src_address.ip6 = ip6->src_address;
      parts->dst_address.ip6 = ip6->dst_address;
      parts->protocol = ip6->protocol;
      l4_offset = sizeof (*ip6);
    }
  else
    {
      ip4_header_t *ip4 = (ip4_header_t *) current;
      u32 ip4_hdr_bytes;

      if (packet_len < sizeof (*ip4))
        return false;

      parts->src_address.ip4 = ip4->src_address;
      parts->dst_address.ip4 = ip4->dst_address;
      parts->protocol = ip4->protocol;
      ip4_hdr_bytes = ip4_header_bytes (ip4);
      if (packet_len < ip4_hdr_bytes)
        return false;
      l4_offset = ip4_hdr_bytes;
    }

  if (parts->protocol == IP_PROTOCOL_TCP)
    {
      tcp_header_t *tcp = (tcp_header_t *) (current + l4_offset);
      if (packet_len < l4_offset + sizeof (*tcp))
        return false;
      parts->src_port = clib_net_to_host_u16 (tcp->src_port);
      parts->dst_port = clib_net_to_host_u16 (tcp->dst_port);
    }
  else if (parts->protocol == IP_PROTOCOL_UDP)
    {
      udp_header_t *udp = (udp_header_t *) (current + l4_offset);
      if (packet_len < l4_offset + sizeof (*udp))
        return false;
      parts->src_port = clib_net_to_host_u16 (udp->src_port);
      parts->dst_port = clib_net_to_host_u16 (udp->dst_port);
    }

  return true;
}

void
subscriber_dp_flow_touch (vlib_main_t *vm, vlib_buffer_t *b, u32 sw_if_index,
                          bool is_ip6, u64 subscriber_id)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  subscriber_dp_flow_key_parts_t parts;
  clib_bihash_kv_48_8_t kv, result;
  subscriber_dp_flow_entry_t *flow;
  uword index;
  f64 now = vlib_time_now (vm);
  u64 bytes = vlib_buffer_length_in_chain (vm, b);

  if (!subscriber_dp_get_flow_parts (vm, b, is_ip6, &parts))
    return;

  subscriber_dp_make_flow_kv (&kv, sw_if_index, &parts.src_address,
                              &parts.dst_address, is_ip6, parts.protocol,
                              parts.src_port, parts.dst_port);

  if (clib_bihash_search_48_8 (&sm->flow_by_key, &kv, &result) == 0)
    {
      flow = pool_elt_at_index (sm->flows, result.value);
      flow->last_seen_at = now;
      flow->subscriber_id = subscriber_id;
      flow->packets++;
      flow->bytes += bytes;
      sm->flow_updates++;
    }
  else
    {
      pool_get_zero (sm->flows, flow);
      index = flow - sm->flows;
      flow->src_address = parts.src_address;
      flow->dst_address = parts.dst_address;
      flow->sw_if_index = sw_if_index;
      flow->subscriber_id = subscriber_id;
      flow->created_at = now;
      flow->last_seen_at = now;
      flow->packets = 1;
      flow->bytes = bytes;
      flow->src_port = parts.src_port;
      flow->dst_port = parts.dst_port;
      flow->protocol = parts.protocol;
      flow->is_ip6 = is_ip6;
      kv.value = index;
      clib_bihash_add_del_48_8 (&sm->flow_by_key, &kv, 1 /* is_add */);
      sm->flow_adds++;
    }

  sm->flow_packets++;
  sm->flow_bytes += bytes;
}

u32
subscriber_dp_age_flows (subscriber_dp_main_t *sm, f64 now)
{
  u32 removed = 0;
  u32 index;
  u32 *to_delete = 0;
  subscriber_dp_flow_entry_t *flow;

  pool_foreach_index (index, sm->flows)
    {
      flow = pool_elt_at_index (sm->flows, index);
      if ((now - flow->last_seen_at) < sm->flow_timeout_sec)
        continue;

      vec_add1 (to_delete, index);
      if (vec_len (to_delete) >= sm->flow_age_max_remove_per_tick)
        break;
    }

  vec_foreach_index (index, to_delete)
    {
      clib_bihash_kv_48_8_t kv;
      flow = pool_elt_at_index (sm->flows, to_delete[index]);
      subscriber_dp_make_flow_kv (&kv, flow->sw_if_index, &flow->src_address,
                                  &flow->dst_address, flow->is_ip6,
                                  flow->protocol, flow->src_port,
                                  flow->dst_port);
      clib_bihash_add_del_48_8 (&sm->flow_by_key, &kv, 0 /* is_add */);
      pool_put_index (sm->flows, to_delete[index]);
      removed++;
    }

  vec_free (to_delete);
  sm->flow_age_runs++;
  sm->flow_age_removed += removed;
  sm->flow_deletes += removed;
  return removed;
}

u32
subscriber_dp_run_subscriber_sync (subscriber_dp_main_t *sm)
{
  sm->subscriber_sync_runs++;
  return 0;
}

int
subscriber_dp_export_stats (subscriber_dp_main_t *sm)
{
  int fd;
  struct sockaddr_un addr;
  u8 *msg = 0;
  int rv = -1;

  if (!sm->stats_socket_path || !sm->stats_socket_path[0])
    return -1;

  fd = -1;
  fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    goto done;

  clib_memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, (char *) sm->stats_socket_path,
           sizeof (addr.sun_path) - 1);

  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    goto done;

  msg = format (0,
                "lookup_hit=%llu lookup_miss=%llu lookup_drop=%llu flow_active=%u flow_add=%llu flow_update=%llu flow_del=%llu\n",
                (unsigned long long) sm->lookup_hits,
                (unsigned long long) sm->lookup_misses,
                (unsigned long long) sm->lookup_drops,
                pool_elts (sm->flows),
                (unsigned long long) sm->flow_adds,
                (unsigned long long) sm->flow_updates,
                (unsigned long long) sm->flow_deletes);

  if (write (fd, msg, vec_len (msg) - 1) >= 0)
    rv = 0;

done:
  if (fd >= 0)
    close (fd);
  vec_free (msg);
  sm->stats_export_runs++;
  if (rv != 0)
    sm->stats_export_failures++;
  return rv;
}

void
subscriber_dp_run_service_maintenance (subscriber_dp_main_t *sm)
{
  sm->policy_refresh_runs++;
  sm->oam_health_runs++;
  sm->cluster_sync_runs++;
}

static u8 *
format_subscriber_dp_flow (u8 *s, va_list *args)
{
  subscriber_dp_flow_entry_t *flow = va_arg (*args, subscriber_dp_flow_entry_t *);
  vnet_main_t *vnm = vnet_get_main ();

  s = format (s, "interface %U proto %u ", format_vnet_sw_if_index_name, vnm,
              flow->sw_if_index, flow->protocol);
  if (flow->is_ip6)
    s = format (s, "%U:%u -> %U:%u", format_ip6_address, &flow->src_address.ip6,
                flow->src_port, format_ip6_address, &flow->dst_address.ip6,
                flow->dst_port);
  else
    s = format (s, "%U:%u -> %U:%u", format_ip4_address, &flow->src_address.ip4,
                flow->src_port, format_ip4_address, &flow->dst_address.ip4,
                flow->dst_port);

  s = format (s, " subscriber-id %llu packets %llu bytes %llu age %.2f",
              (unsigned long long) flow->subscriber_id,
              (unsigned long long) flow->packets,
              (unsigned long long) flow->bytes,
              flow->last_seen_at - flow->created_at);
  return s;
}

static clib_error_t *
subscriber_dp_enable_disable_command_fn (vlib_main_t *vm,
                                         unformat_input_t *input,
                                         vlib_cli_command_t *cmd)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  u32 sw_if_index = ~0;
  bool enable = true;
  bool ip4 = false;
  bool ip6 = false;
  int rv = 0;

  CLIB_UNUSED (vlib_main_t * _vm) = vm;
  CLIB_UNUSED (vlib_cli_command_t * _cmd) = cmd;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "disable"))
        enable = false;
      else if (unformat (input, "ip4"))
        ip4 = true;
      else if (unformat (input, "ip6"))
        ip6 = true;
      else if (unformat (input, "%U", unformat_vnet_sw_interface,
                         sm->vnet_main, &sw_if_index))
        ;
      else
        return clib_error_return (0, "unknown input `%U'",
                                  format_unformat_error, input);
    }

  if (sw_if_index == ~0)
    return clib_error_return (0, "please specify an interface");

  if (!ip4 && !ip6)
    {
      ip4 = true;
      ip6 = true;
    }

  if (ip4)
    {
      rv = subscriber_dp_enable_disable (sw_if_index, false /* is_ip6 */,
                                         enable);
      if (rv)
        goto done;
    }

  if (ip6)
    {
      rv = subscriber_dp_enable_disable (sw_if_index, true /* is_ip6 */,
                                         enable);
      if (rv)
        goto done;
    }

done:
  switch (rv)
    {
    case 0:
      return 0;
    case VNET_API_ERROR_INVALID_SW_IF_INDEX:
      return clib_error_return (0, "invalid interface");
    default:
      return clib_error_return (0,
                                "subscriber_dp_enable_disable returned %d",
                                rv);
    }
}

static clib_error_t *
subscriber_dp_parse_cli_address (unformat_input_t *input,
                                 ip46_address_t *address, bool *is_ip6)
{
  clib_memset (address, 0, sizeof (*address));

  if (unformat (input, "%U", unformat_ip4_address, &address->ip4))
    {
      *is_ip6 = false;
      return 0;
    }

  if (unformat (input, "%U", unformat_ip6_address, &address->ip6))
    {
      *is_ip6 = true;
      return 0;
    }

  return clib_error_return (0, "address must be IPv4 or IPv6");
}

static clib_error_t *
subscriber_dp_subscriber_command_fn (vlib_main_t *vm, unformat_input_t *input,
                                     vlib_cli_command_t *cmd,
                                     subscriber_dp_cli_op_t op)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  u32 sw_if_index = ~0;
  ip46_address_t address;
  bool is_ip6 = false;
  bool address_set = false;
  bool id_set = false;
  u64 subscriber_id = 0;
  unsigned long long cli_subscriber_id = 0;
  int rv = 0;

  CLIB_UNUSED (vlib_main_t * _vm) = vm;
  CLIB_UNUSED (vlib_cli_command_t * _cmd) = cmd;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface, sm->vnet_main,
                    &sw_if_index))
        ;
      else if (unformat (input, "address"))
        {
          clib_error_t *error =
            subscriber_dp_parse_cli_address (input, &address, &is_ip6);
          if (error)
            return error;
          address_set = true;
        }
      else if (unformat (input, "id %llu", &cli_subscriber_id))
        {
          subscriber_id = cli_subscriber_id;
          id_set = true;
        }
      else
        return clib_error_return (0, "unknown input `%U'",
                                  format_unformat_error, input);
    }

  if (sw_if_index == ~0)
    return clib_error_return (0, "please specify an interface");

  if (!address_set)
    return clib_error_return (0, "please specify `address <ip>`");

  if (op != SUBSCRIBER_DP_CLI_DEL && !id_set)
    return clib_error_return (0, "please specify `id <subscriber-id>`");

  if (op == SUBSCRIBER_DP_CLI_ADD)
    rv = subscriber_dp_entry_add (sm, sw_if_index, &address, is_ip6,
                                  subscriber_id);
  else if (op == SUBSCRIBER_DP_CLI_UPDATE)
    rv = subscriber_dp_entry_update (sm, sw_if_index, &address, is_ip6,
                                     subscriber_id);
  else
    rv = subscriber_dp_entry_del (sm, sw_if_index, &address, is_ip6);

  switch (rv)
    {
    case 0:
      return 0;
    case VNET_API_ERROR_VALUE_EXIST:
      return clib_error_return (0, "subscriber entry already exists");
    case VNET_API_ERROR_NO_SUCH_ENTRY:
      return clib_error_return (0, "subscriber entry does not exist");
    default:
      return clib_error_return (0, "subscriber operation returned %d", rv);
    }
}

static clib_error_t *
subscriber_dp_subscriber_add_command_fn (vlib_main_t *vm,
                                         unformat_input_t *input,
                                         vlib_cli_command_t *cmd)
{
  return subscriber_dp_subscriber_command_fn (vm, input, cmd,
                                              SUBSCRIBER_DP_CLI_ADD);
}

static clib_error_t *
subscriber_dp_subscriber_update_command_fn (vlib_main_t *vm,
                                            unformat_input_t *input,
                                            vlib_cli_command_t *cmd)
{
  return subscriber_dp_subscriber_command_fn (vm, input, cmd,
                                              SUBSCRIBER_DP_CLI_UPDATE);
}

static clib_error_t *
subscriber_dp_subscriber_del_command_fn (vlib_main_t *vm,
                                         unformat_input_t *input,
                                         vlib_cli_command_t *cmd)
{
  return subscriber_dp_subscriber_command_fn (vm, input, cmd,
                                              SUBSCRIBER_DP_CLI_DEL);
}

static clib_error_t *
show_subscriber_dp_command_fn (vlib_main_t *vm, unformat_input_t *input,
                               vlib_cli_command_t *cmd)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  subscriber_dp_entry_t *entry;
  bool show_interfaces = false;
  bool show_flows = false;
  bool show_services = false;
  u32 sw_if_index;

  CLIB_UNUSED (vlib_cli_command_t * _cmd) = cmd;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "interfaces"))
        show_interfaces = true;
      else if (unformat (input, "flows"))
        show_flows = true;
      else if (unformat (input, "services"))
        show_services = true;
      else
        return clib_error_return (0, "unknown input `%U'",
                                  format_unformat_error, input);
    }

  vlib_cli_output (vm,
                   "ops: add %llu update %llu del %llu lookup-hit %llu lookup-miss %llu lookup-drop %llu flow-active %u flow-add %llu flow-update %llu flow-del %llu",
                   (unsigned long long) sm->add_ops,
                   (unsigned long long) sm->update_ops,
                   (unsigned long long) sm->del_ops,
                   (unsigned long long) sm->lookup_hits,
                   (unsigned long long) sm->lookup_misses,
                   (unsigned long long) sm->lookup_drops, pool_elts (sm->flows),
                   (unsigned long long) sm->flow_adds,
                   (unsigned long long) sm->flow_updates,
                   (unsigned long long) sm->flow_deletes);

  if (show_interfaces)
    {
      vlib_cli_output (vm, "feature-enabled interfaces:");

      clib_bitmap_foreach (sw_if_index, sm->feature_enabled_ip4_by_sw_if)
        {
          vlib_cli_output (vm, "  ip4 %U",
                           format_vnet_sw_if_index_name, sm->vnet_main,
                           sw_if_index);
        }

      clib_bitmap_foreach (sw_if_index, sm->feature_enabled_ip6_by_sw_if)
        {
          vlib_cli_output (vm, "  ip6 %U",
                           format_vnet_sw_if_index_name, sm->vnet_main,
                           sw_if_index);
        }
    }

  if (show_services)
    {
      vlib_cli_output (vm,
                       "services: subscriber-sync runs %llu processed %llu flow-aging runs %llu removed %llu stats-export runs %llu failures %llu policy-refresh %llu oam-health %llu cluster-sync %llu",
                       (unsigned long long) sm->subscriber_sync_runs,
                       (unsigned long long) sm->subscriber_sync_processed,
                       (unsigned long long) sm->flow_age_runs,
                       (unsigned long long) sm->flow_age_removed,
                       (unsigned long long) sm->stats_export_runs,
                       (unsigned long long) sm->stats_export_failures,
                       (unsigned long long) sm->policy_refresh_runs,
                       (unsigned long long) sm->oam_health_runs,
                       (unsigned long long) sm->cluster_sync_runs);
      vlib_cli_output (vm,
                       "config: subscriber-sync %.1fs flow-aging %.1fs timeout %.1fs max-remove %u stats-export %.1fs socket %s service-loop %.1fs",
                       sm->subscriber_sync_interval_sec,
                       sm->flow_age_interval_sec,
                       sm->flow_timeout_sec,
                       sm->flow_age_max_remove_per_tick,
                       sm->stats_export_interval_sec,
                       sm->stats_socket_path ?
                         (char *) sm->stats_socket_path : "(disabled)",
                       sm->service_interval_sec);
    }

  if (pool_elts (sm->entries) == 0)
    vlib_cli_output (vm, "no subscriber entries");
  else
    pool_foreach (entry, sm->entries)
      {
        vlib_cli_output (vm, "%U", format_subscriber_dp_entry, entry);
      }

  if (show_flows)
    {
      subscriber_dp_flow_entry_t *flow;

      if (pool_elts (sm->flows) == 0)
        vlib_cli_output (vm, "no active flows");
      else
        pool_foreach (flow, sm->flows)
          {
            vlib_cli_output (vm, "%U", format_subscriber_dp_flow, flow);
          }
    }

  return 0;
}

VLIB_CLI_COMMAND (subscriber_dp_enable_disable_command, static) = {
  .path = "subscriber-dp enable-disable",
  .short_help =
    "subscriber-dp enable-disable <interface> [ip4] [ip6] [disable]",
  .function = subscriber_dp_enable_disable_command_fn,
};

VLIB_CLI_COMMAND (subscriber_dp_subscriber_add_command, static) = {
  .path = "subscriber-dp subscriber add",
  .short_help =
    "subscriber-dp subscriber add <interface> address <ip> id <subscriber-id>",
  .function = subscriber_dp_subscriber_add_command_fn,
};

VLIB_CLI_COMMAND (subscriber_dp_subscriber_update_command, static) = {
  .path = "subscriber-dp subscriber update",
  .short_help =
    "subscriber-dp subscriber update <interface> address <ip> id <subscriber-id>",
  .function = subscriber_dp_subscriber_update_command_fn,
};

VLIB_CLI_COMMAND (subscriber_dp_subscriber_del_command, static) = {
  .path = "subscriber-dp subscriber del",
  .short_help =
    "subscriber-dp subscriber del <interface> address <ip>",
  .function = subscriber_dp_subscriber_del_command_fn,
};

VLIB_CLI_COMMAND (show_subscriber_dp_command, static) = {
  .path = "show subscriber-dp",
  .short_help = "show subscriber-dp [interfaces] [flows] [services]",
  .function = show_subscriber_dp_command_fn,
};
