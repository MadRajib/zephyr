# SPDX-License-Identifier: Apache-2.0

if(CONFIG_ETH_CH9120)
  zephyr_include_directories(./)

  zephyr_library_sources(
    ch9120.c
    ch9120_socket.c
  )
endif()
