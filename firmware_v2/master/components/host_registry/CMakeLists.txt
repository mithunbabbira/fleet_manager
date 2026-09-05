set(FLEET_REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
# Prefer pre-copied catalog; regenerate only if script exists and succeeds.
if(EXISTS "${FLEET_REPO_ROOT}/scripts/gen_fleet_manifests.py")
    execute_process(
        COMMAND python3 ${FLEET_REPO_ROOT}/scripts/gen_fleet_manifests.py
        WORKING_DIRECTORY ${FLEET_REPO_ROOT}
        RESULT_VARIABLE _fleet_manifest_rc
    )
    if(NOT _fleet_manifest_rc EQUAL 0)
        message(WARNING "gen_fleet_manifests.py failed (${_fleet_manifest_rc}); using checked-in catalog")
    endif()
endif()

idf_component_register(SRCS "host_registry.c" "fleet_manifest_catalog.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fleet_protocol esp_timer)
