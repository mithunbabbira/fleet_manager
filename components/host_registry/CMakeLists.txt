set(FLEET_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
execute_process(
    COMMAND python3 ${FLEET_REPO_ROOT}/scripts/gen_fleet_manifests.py
    WORKING_DIRECTORY ${FLEET_REPO_ROOT}
    RESULT_VARIABLE _fleet_manifest_rc
)
if(NOT _fleet_manifest_rc EQUAL 0)
    message(FATAL_ERROR "gen_fleet_manifests.py failed (${_fleet_manifest_rc})")
endif()

idf_component_register(SRCS "host_registry.c" "fleet_manifest_catalog.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fleet_protocol telemetry_bus esp_timer)
