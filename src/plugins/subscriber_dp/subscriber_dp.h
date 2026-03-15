#ifndef included_subscriber_dp_h
#define included_subscriber_dp_h

#include <stdbool.h>
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vppinfra/bihash_24_8.h>
#include <vppinfra/bihash_48_8.h>
#include <vppinfra/bitmap.h>
#include <vppinfra/pool.h>

typedef struct
{
  ip46_address_t address;
  u32 sw_if_index;
  u64 subscriber_id;
  u8 is_ip6;
} subscriber_dp_entry_t;

typedef struct
{
  u64 subscriber_id;
  u8 subscriber_valid;
} subscriber_dp_buffer_opaque_t;

typedef struct
{
  ip46_address_t src_address;
  ip46_address_t dst_address;
  u32 sw_if_index;
  u64 subscriber_id;
  f64 created_at;
  f64 last_seen_at;
  u64 packets;
  u64 bytes;
  u16 src_port;
  u16 dst_port;
  u8 protocol;
  u8 is_ip6;
} subscriber_dp_flow_entry_t;

typedef struct
{
  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
  u32 msg_id_base;

  clib_bihash_24_8_t subscriber_by_key;
  subscriber_dp_entry_t *entries;
  clib_bitmap_t *feature_enabled_ip4_by_sw_if;
  clib_bitmap_t *feature_enabled_ip6_by_sw_if;

  clib_bihash_48_8_t flow_by_key;
  subscriber_dp_flow_entry_t *flows;

  f64 subscriber_sync_interval_sec;
  f64 flow_age_interval_sec;
  f64 stats_export_interval_sec;
  f64 service_interval_sec;
  f64 flow_timeout_sec;

  u32 subscriber_sync_max_per_tick;
  u32 flow_age_max_remove_per_tick;
  u8 *stats_socket_path;

  u64 add_ops;
  u64 del_ops;
  u64 update_ops;
  u64 lookup_hits;
  u64 lookup_misses;
  u64 lookup_drops;

  u64 flow_adds;
  u64 flow_updates;
  u64 flow_deletes;
  u64 flow_packets;
  u64 flow_bytes;
  u64 subscriber_sync_runs;
  u64 subscriber_sync_processed;
  u64 flow_age_runs;
  u64 flow_age_removed;
  u64 stats_export_runs;
  u64 stats_export_failures;
  u64 policy_refresh_runs;
  u64 oam_health_runs;
  u64 cluster_sync_runs;
} subscriber_dp_main_t;

extern subscriber_dp_main_t subscriber_dp_main;

#define SUBSCRIBER_DP_PLUGIN_VERSION_MAJOR 0
#define SUBSCRIBER_DP_PLUGIN_VERSION_MINOR 1

#define subscriber_dp_buffer_opaque(b) \
  ((subscriber_dp_buffer_opaque_t *) ((b)->opaque2))

static_always_inline void
subscriber_dp_address_from_ip4 (ip46_address_t *dst, const ip4_address_t *src)
{
  clib_memset (dst, 0, sizeof (*dst));
  dst->ip4 = *src;
}

static_always_inline void
subscriber_dp_make_kv (clib_bihash_kv_24_8_t *kv, u32 sw_if_index,
                       const ip46_address_t *address, bool is_ip6)
{
  clib_memset (kv, 0, sizeof (*kv));
  kv->key[0] = ((u64) sw_if_index << 32) | (u64) is_ip6;
  clib_memcpy_fast (&kv->key[1], address, sizeof (*address));
}

static_always_inline void
subscriber_dp_make_flow_kv (clib_bihash_kv_48_8_t *kv, u32 sw_if_index,
                            const ip46_address_t *src_address,
                            const ip46_address_t *dst_address, bool is_ip6,
                            u8 protocol, u16 src_port, u16 dst_port)
{
  clib_memset (kv, 0, sizeof (*kv));
  kv->key[0] = ((u64) sw_if_index << 32) | ((u64) protocol << 16) | is_ip6;
  kv->key[1] = ((u64) src_port << 16) | dst_port;

  if (is_ip6)
    {
      clib_memcpy_fast (&kv->key[2], &src_address->ip6, sizeof (ip6_address_t));
      clib_memcpy_fast (&kv->key[4], &dst_address->ip6, sizeof (ip6_address_t));
    }
  else
    {
      kv->key[2] = src_address->ip4.as_u32;
      kv->key[3] = dst_address->ip4.as_u32;
    }
}

int subscriber_dp_enable_disable (u32 sw_if_index, bool is_ip6, bool enable);
int subscriber_dp_entry_add (subscriber_dp_main_t *sm, u32 sw_if_index,
                             const ip46_address_t *address, bool is_ip6,
                             u64 subscriber_id);
int subscriber_dp_entry_del (subscriber_dp_main_t *sm, u32 sw_if_index,
                             const ip46_address_t *address, bool is_ip6);
int subscriber_dp_entry_update (subscriber_dp_main_t *sm, u32 sw_if_index,
                                const ip46_address_t *address, bool is_ip6,
                                u64 subscriber_id);
subscriber_dp_entry_t *subscriber_dp_lookup (subscriber_dp_main_t *sm,
                                             u32 sw_if_index,
                                             const ip46_address_t *address,
                                             bool is_ip6);
void subscriber_dp_flow_touch (vlib_main_t *vm, vlib_buffer_t *b,
                               u32 sw_if_index, bool is_ip6,
                               u64 subscriber_id);
u32 subscriber_dp_age_flows (subscriber_dp_main_t *sm, f64 now);
u32 subscriber_dp_run_subscriber_sync (subscriber_dp_main_t *sm);
int subscriber_dp_export_stats (subscriber_dp_main_t *sm);
void subscriber_dp_run_service_maintenance (subscriber_dp_main_t *sm);

#endif
