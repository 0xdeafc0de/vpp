#include <vnet/vnet.h>
#include <vlibmemory/api.h>
#include <vlibapi/api.h>

#include "subscriber_dp.h"

#include <subscriber_dp/subscriber_dp.api_enum.h>
#include <subscriber_dp/subscriber_dp.api_types.h>

#define REPLY_MSG_ID_BASE subscriber_dp_main.msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
subscriber_dp_decode_address (ip46_address_t *addr, const u8 *src, bool is_ip6)
{
  clib_memset (addr, 0, sizeof (*addr));
  if (is_ip6)
    clib_memcpy_fast (&addr->ip6, src, sizeof (addr->ip6));
  else
    clib_memcpy_fast (&addr->ip4, src, sizeof (addr->ip4));
}

static void
vl_api_subscriber_dp_enable_disable_t_handler (
  vl_api_subscriber_dp_enable_disable_t *mp)
{
  vl_api_subscriber_dp_enable_disable_reply_t *rmp;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  rv = subscriber_dp_enable_disable (ntohl (mp->sw_if_index), mp->is_ip6,
                                     mp->enable);

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_SUBSCRIBER_DP_ENABLE_DISABLE_REPLY);
}

static void
vl_api_subscriber_dp_subscriber_add_t_handler (
  vl_api_subscriber_dp_subscriber_add_t *mp)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  vl_api_subscriber_dp_subscriber_add_reply_t *rmp;
  ip46_address_t address;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  subscriber_dp_decode_address (&address, mp->address, mp->is_ip6);
  rv = subscriber_dp_entry_add (sm, ntohl (mp->sw_if_index), &address,
                                mp->is_ip6,
                                clib_net_to_host_u64 (mp->subscriber_id));

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_SUBSCRIBER_DP_SUBSCRIBER_ADD_REPLY);
}

static void
vl_api_subscriber_dp_subscriber_del_t_handler (
  vl_api_subscriber_dp_subscriber_del_t *mp)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  vl_api_subscriber_dp_subscriber_del_reply_t *rmp;
  ip46_address_t address;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  subscriber_dp_decode_address (&address, mp->address, mp->is_ip6);
  rv = subscriber_dp_entry_del (sm, ntohl (mp->sw_if_index), &address,
                                mp->is_ip6);

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_SUBSCRIBER_DP_SUBSCRIBER_DEL_REPLY);
}

static void
vl_api_subscriber_dp_subscriber_update_t_handler (
  vl_api_subscriber_dp_subscriber_update_t *mp)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  vl_api_subscriber_dp_subscriber_update_reply_t *rmp;
  ip46_address_t address;
  int rv;

  VALIDATE_SW_IF_INDEX (mp);

  subscriber_dp_decode_address (&address, mp->address, mp->is_ip6);
  rv = subscriber_dp_entry_update (sm, ntohl (mp->sw_if_index), &address,
                                   mp->is_ip6,
                                   clib_net_to_host_u64 (mp->subscriber_id));

  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_SUBSCRIBER_DP_SUBSCRIBER_UPDATE_REPLY);
}

static void
send_subscriber_dp_subscriber_details (vl_api_registration_t *rp, u32 context,
                                       const subscriber_dp_entry_t *entry)
{
  vl_api_subscriber_dp_subscriber_details_t *mp;

  mp = vl_msg_api_alloc (sizeof (*mp));
  clib_memset (mp, 0, sizeof (*mp));
  mp->_vl_msg_id =
    ntohs (VL_API_SUBSCRIBER_DP_SUBSCRIBER_DETAILS + subscriber_dp_main.msg_id_base);
  mp->context = context;
  mp->sw_if_index = htonl (entry->sw_if_index);
  mp->subscriber_id = clib_host_to_net_u64 (entry->subscriber_id);
  mp->is_ip6 = entry->is_ip6;

  if (entry->is_ip6)
    clib_memcpy_fast (mp->address, &entry->address.ip6, sizeof (entry->address.ip6));
  else
    clib_memcpy_fast (mp->address, &entry->address.ip4, sizeof (entry->address.ip4));

  vl_api_send_msg (rp, (u8 *) mp);
}

static void
vl_api_subscriber_dp_subscriber_dump_t_handler (
  vl_api_subscriber_dp_subscriber_dump_t *mp)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  vl_api_registration_t *rp;
  subscriber_dp_entry_t *entry;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  pool_foreach (entry, sm->entries)
    {
      send_subscriber_dp_subscriber_details (rp, mp->context, entry);
    }
}

#include <subscriber_dp/subscriber_dp.api.c>

static clib_error_t *
subscriber_dp_api_init (vlib_main_t *vm)
{
  subscriber_dp_main.msg_id_base = setup_message_id_table ();
  return 0;
}

VLIB_API_INIT_FUNCTION (subscriber_dp_api_init);
