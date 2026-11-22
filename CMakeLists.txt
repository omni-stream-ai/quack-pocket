# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.4")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Resolve the checked-in dependency lock without probing unrelated newer manifests.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} "0")
set(IDF_TARGET esp32s3 CACHE STRING "Omni device target")
set(SDKCONFIG_DEFAULTS sdkconfig.waveshare-v2 CACHE STRING "Omni board configuration")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(omni_epaper)

