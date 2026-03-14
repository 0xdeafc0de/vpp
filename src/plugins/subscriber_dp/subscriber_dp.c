#include <vlib/vlib.h>
#include <vlib/threads.h>

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

static clib_error_t *
subscriber_dp_init (vlib_main_t *vm)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;

  sm->vlib_main = vm;
  sm->vnet_main = vnet_get_main ();

  clib_bihash_init_24_8 (&sm->subscriber_by_key, "subscriber-dp-subscriber-by-key",
                         1024 /* buckets */, 1 << 20 /* memory */);

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

  return vnet_feature_enable_disable (is_ip6 ? "ip6-unicast" : "ip4-unicast",
                                      is_ip6 ? "subscriber-dp-ip6" : "subscriber-dp-ip4",
                                      sw_if_index, enable, 0, 0);
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

  CLIB_UNUSED (vlib_cli_command_t * _cmd) = cmd;

  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    return clib_error_return (0, "unknown input `%U'",
                              format_unformat_error, input);

  vlib_cli_output (vm,
                   "ops: add %llu update %llu del %llu lookup-hit %llu lookup-miss %llu",
                   (unsigned long long) sm->add_ops,
                   (unsigned long long) sm->update_ops,
                   (unsigned long long) sm->del_ops,
                   (unsigned long long) sm->lookup_hits,
                   (unsigned long long) sm->lookup_misses);

  if (pool_elts (sm->entries) == 0)
    {
      vlib_cli_output (vm, "no subscriber entries");
      return 0;
    }

  pool_foreach (entry, sm->entries)
    {
      vlib_cli_output (vm, "%U", format_subscriber_dp_entry, entry);
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
  .short_help = "show subscriber-dp",
  .function = show_subscriber_dp_command_fn,
};
