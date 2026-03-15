#include <vlib/vlib.h>

#include "subscriber_dp.h"

static uword
subscriber_dp_subscriber_sync_process (vlib_main_t *vm,
                                       vlib_node_runtime_t *rt,
                                       vlib_frame_t *f)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  CLIB_UNUSED (vlib_node_runtime_t * _rt) = rt;
  CLIB_UNUSED (vlib_frame_t * _f) = f;

  while (1)
    {
      vlib_process_wait_for_event_or_clock (vm,
                                            sm->subscriber_sync_interval_sec);
      sm->subscriber_sync_processed += subscriber_dp_run_subscriber_sync (sm);
    }

  return 0;
}

static uword
subscriber_dp_flow_age_process (vlib_main_t *vm, vlib_node_runtime_t *rt,
                                vlib_frame_t *f)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  CLIB_UNUSED (vlib_node_runtime_t * _rt) = rt;
  CLIB_UNUSED (vlib_frame_t * _f) = f;

  while (1)
    {
      vlib_process_wait_for_event_or_clock (vm, sm->flow_age_interval_sec);
      subscriber_dp_age_flows (sm, vlib_time_now (vm));
    }

  return 0;
}

static uword
subscriber_dp_stats_export_process (vlib_main_t *vm, vlib_node_runtime_t *rt,
                                    vlib_frame_t *f)
{
  subscriber_dp_main_t *sm = &subscriber_dp_main;
  CLIB_UNUSED (vlib_node_runtime_t * _rt) = rt;
  CLIB_UNUSED (vlib_frame_t * _f) = f;

  while (1)
    {
      vlib_process_wait_for_event_or_clock (vm,
                                            sm->stats_export_interval_sec);
      subscriber_dp_export_stats (sm);
      subscriber_dp_run_service_maintenance (sm);
    }

  return 0;
}

VLIB_REGISTER_NODE (subscriber_dp_subscriber_sync_process_node, static) = {
  .function = subscriber_dp_subscriber_sync_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "subscriber-dp-subscriber-sync-process",
};

VLIB_REGISTER_NODE (subscriber_dp_flow_age_process_node, static) = {
  .function = subscriber_dp_flow_age_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "subscriber-dp-flow-age-process",
};

VLIB_REGISTER_NODE (subscriber_dp_stats_export_process_node, static) = {
  .function = subscriber_dp_stats_export_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "subscriber-dp-stats-export-process",
};
