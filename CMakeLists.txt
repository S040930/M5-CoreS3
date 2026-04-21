# CoreS3 AirPlay Core
cmake_minimum_required(VERSION 3.16)

# Read firmware version from version.txt
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

set(EXCLUDE_COMPONENTS
    airplay_state
    settings_core
    ui_core
    ui_display
)

set(_IDF_PATH "")
if(DEFINED IDF_PATH AND NOT "${IDF_PATH}" STREQUAL "")
	set(_IDF_PATH "${IDF_PATH}")
elseif(DEFINED ENV{IDF_PATH} AND NOT "$ENV{IDF_PATH}" STREQUAL "")
	set(_IDF_PATH "$ENV{IDF_PATH}")
elseif(DEFINED ENV{HOME})
	# Fallback for PlatformIO-managed ESP-IDF installations.
	set(_pio_idf_path "$ENV{HOME}/.platformio/packages/framework-espidf")
	if(EXISTS "${_pio_idf_path}/tools/cmake/project.cmake")
		set(_IDF_PATH "${_pio_idf_path}")
	endif()
endif()

if(EXISTS "${_IDF_PATH}/tools/cmake/project.cmake")
	include("${_IDF_PATH}/tools/cmake/project.cmake")
else()
	message(FATAL_ERROR "ESP-IDF not found. Set IDF_PATH (CMake variable or environment variable), or install ESP-IDF via PlatformIO at ~/.platformio/packages/framework-espidf.")
endif()

project(cores3_airplay_core VERSION ${PROJECT_VER})
