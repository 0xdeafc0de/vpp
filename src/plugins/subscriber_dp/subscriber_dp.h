#ifndef included_subscriber_dp_h
#define included_subscriber_dp_h

#include <stdbool.h>
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vppinfra/bihash_24_8.h>
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
  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
  u32 msg_id_base;

  clib_bihash_24_8_t subscriber_by_key;
  subscriber_dp_entry_t *entries;
  clib_bitmap_t *feature_enabled_ip4_by_sw_if;
  clib_bitmap_t *feature_enabled_ip6_by_sw_if;

  u64 add_ops;
  u64 del_ops;
  u64 update_ops;
  u64 lookup_hits;
  u64 lookup_misses;
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

#endif
