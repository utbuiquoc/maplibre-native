cmake_minimum_required(VERSION 3.16)

project(MapLibreNativeOhosSample)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MAPLIBRE_NATIVE_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.."
    CACHE PATH
    "Path to the MapLibre Native checkout"
)
get_filename_component(MAPLIBRE_NATIVE_ROOT "${MAPLIBRE_NATIVE_ROOT}" ABSOLUTE)

set(MLN_OHOS_SAMPLE_RENDERER "Vulkan" CACHE STRING "OHOS renderer backend: Vulkan or OpenGL")

add_subdirectory(
    "${MAPLIBRE_NATIVE_ROOT}/platform/ohos/napi"
    "${CMAKE_BINARY_DIR}/ohos-napi"
)
