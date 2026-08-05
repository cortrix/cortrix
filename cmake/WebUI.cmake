# Cleaning: Web UI Build Integration
# Builds the React SPA and copies output to web/static/ for embedding in cortrix-server

if(EXISTS ${CMAKE_SOURCE_DIR}/web/package.json)
    message(STATUS "Web UI found, configuring frontend build...")

    find_program(NPM_EXECUTABLE NAMES pnpm npm yarn)
    if(NOT NPM_EXECUTABLE)
        message(WARNING "npm/pnpm/yarn not found, skipping Web UI build")
    else()
        message(STATUS "Using package manager: ${NPM_EXECUTABLE}")

        # Custom target: web-ui-install
        add_custom_target(web-ui-install
            COMMAND ${NPM_EXECUTABLE} install
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/web
            COMMENT "Installing Web UI dependencies..."
        )

        # Custom target: web-ui-build
        add_custom_target(web-ui-build
            COMMAND ${NPM_EXECUTABLE} run build
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/web
            COMMENT "Building Web UI (Vite)..."
            DEPENDS web-ui-install
        )

        # Make cortrix-server depend on web-ui if target exists
        if(TARGET cortrix-server)
            add_dependencies(cortrix-server web-ui-build)
        endif()
    endif()
else()
    message(STATUS "Web UI not found at ${CMAKE_SOURCE_DIR}/web/, skipping")
endif()
