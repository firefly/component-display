cmake_minimum_required(VERSION 3.16)

idf_component_register(
  SRCS
    "firefly-display.c"
  INCLUDE_DIRS
    "include"
  REQUIRES
    driver
)
