# SPDX-License-Identifier: Apache-2.0

if(CONFIG_WIFI_CH9120)
  zephyr_include_directories(./)

  zephyr_library_sources(
    ch9120.c
    )
endif()
