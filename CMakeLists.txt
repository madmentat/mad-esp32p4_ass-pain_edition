cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(JC4880P443C_Demo)

# ---------------------------------------------------------------------------
# esp-wifi-remote Kconfig orsource fix
# https://github.com/espressif/esp-wifi-remote/issues/85
#
# esp_wifi_remote Kconfig uses: orsource "./idf_v$ESP_IDF_VERSION/Kconfig.slave_select.in"
# Kconfig does NOT expand $ESP_IDF_VERSION, so it looks for literally
# "idf_v5.5.4/" which does not exist. Only "idf_v5.5/" exists.
# This causes SLAVE_IDF_TARGET to never be defined, and esp_hosted 2.7.0
# fails with #error "Unknown Slave Target".
#
# Fix: copy idf_vX.Y/ -> idf_vX.Y.Z/ after component fetch.
# Runs AFTER project() so the component manager has already fetched components.
# Survives full clean + reconfigure + project transfer.
# ---------------------------------------------------------------------------
macro(_wifi_remote_fix_idf_version_copy)
    set(_WIFI_REMOTE_DIR "")
    foreach(_comp_dir ${CMAKE_SOURCE_DIR}/managed_components
                      ${CMAKE_SOURCE_DIR}/components)
        if(EXISTS "${_comp_dir}/espressif__esp_wifi_remote/Kconfig")
            set(_WIFI_REMOTE_DIR "${_comp_dir}/espressif__esp_wifi_remote")
            break()
        endif()
    endforeach()

    if(NOT _WIFI_REMOTE_DIR)
        return()
    endif()

    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _match "${IDF_VERSION}")
    set(_IDF_SHORT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
    set(_IDF_FULL "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

    set(_SRC_DIR "${_WIFI_REMOTE_DIR}/idf_v${_IDF_SHORT}")
    set(_DST_DIR "${_WIFI_REMOTE_DIR}/idf_v${_IDF_FULL}")

    if(EXISTS "${_SRC_DIR}" AND NOT EXISTS "${_DST_DIR}")
        message(STATUS "esp-wifi-remote Kconfig fix: copying idf_v${_IDF_SHORT} -> idf_v${_IDF_FULL}")
        file(MAKE_DIRECTORY "${_DST_DIR}")
        file(COPY "${_SRC_DIR}/" DESTINATION "${_DST_DIR}")
    endif()
endmacro()

_wifi_remote_fix_idf_version_copy()
