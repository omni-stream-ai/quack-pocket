# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.0.36")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Resolve the checked-in dependency lock without probing unrelated newer manifests.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} "0")
set(IDF_TARGET esp32s3 CACHE STRING "Omni device target")
set(SDKCONFIG_DEFAULTS sdkconfig.waveshare-v2 CACHE STRING "Omni board configuration")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(omni_epaper)


# esp-wifi-connect 2.6.2 schedules another full scan after just 10 ms.
# Build the pinned station source with a battery-friendly retry interval.
idf_component_get_property(station_lib 78__esp-wifi-connect COMPONENT_LIB)
idf_component_get_property(station_dir 78__esp-wifi-connect COMPONENT_DIR)
set(station_src "${station_dir}/wifi_station.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${station_src}")
file(READ "${station_src}" station_code)
string(REPLACE "timer_handle_, 10 * 1000)" "timer_handle_, 15 * 1000 * 1000)" station_code "${station_code}")
set(station_build_src "${CMAKE_CURRENT_BINARY_DIR}/wifi_station.cc")
file(CONFIGURE OUTPUT "${station_build_src}" CONTENT "${station_code}" @ONLY)
get_target_property(station_sources ${station_lib} SOURCES)
list(REMOVE_ITEM station_sources "${station_src}")
set_property(TARGET ${station_lib} PROPERTY SOURCES "${station_sources};${station_build_src}")
