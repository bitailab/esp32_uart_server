# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components/ $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
# set(EXTRA_COMPONENT_DIRS )
# set(EXTRA_COMPONENT_DIRS components/)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_uart_server)
