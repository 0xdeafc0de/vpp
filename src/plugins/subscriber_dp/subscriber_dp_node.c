#include <vlib/vlib.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>

#include "subscriber_dp.h"

typedef struct
{
  u32 sw_if_index;
  u64 subscriber_id;
  u8 hit;
  u8 is_ip6;
} subscriber_dp_trace_t;

static u8 *
format_subscriber_dp_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  subscriber_dp_trace_t *t = va_arg (*args, subscriber_dp_trace_t *);

  s = format (s, "sw_if_index %u hit %u is_ip6 %u subscriber_id %llu",
              t->sw_if_index, t->hit, t->is_ip6,
              (unsigned long long) t->subscriber_id);
  return s;
}

static_always_inline void
subscriber_dp_process_ip4 (vlib_buffer_t *b, u32 sw_if_index)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  ip4_header_t *ip4;
  ip46_address_t key;
  subscriber_dp_entry_t *entry;
  subscriber_dp_buffer_opaque_t *meta;

  ip4 = vlib_buffer_get_current (b);
  subscriber_dp_address_from_ip4 (&key, &ip4->src_address);
  entry = subscriber_dp_lookup (sm, sw_if_index, &key, false /* is_ip6 */);

  meta = subscriber_dp_buffer_opaque (b);
  meta->subscriber_valid = (entry != 0);
  meta->subscriber_id = entry ? entry->subscriber_id : 0;
}

static_always_inline void
subscriber_dp_process_ip6 (vlib_buffer_t *b, u32 sw_if_index)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  ip6_header_t *ip6;
  ip46_address_t key;
  subscriber_dp_entry_t *entry;
  subscriber_dp_buffer_opaque_t *meta;

  ip6 = vlib_buffer_get_current (b);
  clib_memset (&key, 0, sizeof (key));
  key.ip6 = ip6->src_address;
  entry = subscriber_dp_lookup (sm, sw_if_index, &key, true /* is_ip6 */);

  meta = subscriber_dp_buffer_opaque (b);
  meta->subscriber_valid = (entry != 0);
  meta->subscriber_id = entry ? entry->subscriber_id : 0;
}

static_always_inline uword
subscriber_dp_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
                      vlib_frame_t *frame, bool is_ip6)
{
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b = bufs;
  u16 nexts[VLIB_FRAME_SIZE], *next = nexts;
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;

  vlib_get_buffers (vm, from, b, frame->n_vectors);

  while (n_left > 0)
    {
      u32 sw_if_index0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];

      if (is_ip6)
        subscriber_dp_process_ip6 (b[0], sw_if_index0);
      else
        subscriber_dp_process_ip4 (b[0], sw_if_index0);

      vnet_feature_next_u16 (next, b[0]);

      if (PREDICT_FALSE (b[0]->flags & VLIB_BUFFER_IS_TRACED))
        {
          subscriber_dp_trace_t *t;
          subscriber_dp_buffer_opaque_t *meta = subscriber_dp_buffer_opaque (b[0]);

          t = vlib_add_trace (vm, node, b[0], sizeof (*t));
          t->sw_if_index = sw_if_index0;
          t->subscriber_id = meta->subscriber_id;
          t->hit = meta->subscriber_valid;
          t->is_ip6 = is_ip6;
        }

      b++;
      next++;
      n_left--;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);
  return frame->n_vectors;
}

VLIB_NODE_FN (subscriber_dp_ip4_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return subscriber_dp_inline (vm, node, frame, false /* is_ip6 */);
}

VLIB_NODE_FN (subscriber_dp_ip6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return subscriber_dp_inline (vm, node, frame, true /* is_ip6 */);
}

VLIB_REGISTER_NODE (subscriber_dp_ip4_node) = {
  .name = "subscriber-dp-ip4",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .flags = VLIB_NODE_FLAG_TRACE_SUPPORTED,
  .format_trace = format_subscriber_dp_trace,
};

VLIB_REGISTER_NODE (subscriber_dp_ip6_node) = {
  .name = "subscriber-dp-ip6",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .flags = VLIB_NODE_FLAG_TRACE_SUPPORTED,
  .format_trace = format_subscriber_dp_trace,
};

VNET_FEATURE_INIT (subscriber_dp_ip4_feat, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "subscriber-dp-ip4",
  .runs_after = VNET_FEATURES ("ip4-full-reassembly-feature",
                               "ip4-sv-reassembly-feature"),
};

VNET_FEATURE_INIT (subscriber_dp_ip6_feat, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "subscriber-dp-ip6",
  .runs_after = VNET_FEATURES ("ip6-full-reassembly-feature",
                               "ip6-sv-reassembly-feature"),
};
