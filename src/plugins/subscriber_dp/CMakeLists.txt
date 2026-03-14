# Copyright (c) 2026

add_vpp_plugin(subscriber_dp
  SOURCES
  subscriber_dp.c
  subscriber_dp_api.c
  subscriber_dp_node.c
  plugin.c

  MULTIARCH_SOURCES
  subscriber_dp_node.c

  API_FILES
  subscriber_dp.api

  INSTALL_HEADERS
  subscriber_dp.h
)
