cmake_minimum_required(VERSION 3.14)
project(wal_viewer)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw
  GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

# ImGui
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui
  GIT_TAG        v1.90.1
)
FetchContent_MakeAvailable(imgui)

# nlohmann_json
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG        v3.11.2
)
FetchContent_MakeAvailable(json)

# stb
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nlohmann/stb
  GIT_TAG        master
)
# Note: nlohmann/stb is a mirror, or we can use local or other source. 
# actually let's use a reliable source or just download the single header if needed.
# FetchContent is fine for standard repos. 'nlohmann/stb' might not be standard.
# Let's use simple FetchContent for stb from a known mirror or just use the header if it was easier.
# But sticking to the plan:
FetchContent_Declare(
  stb_image
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(stb_image)

set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)

# App Sources
set(APP_SOURCES
    "src/main.cpp"
    "src/imgui_hex.cpp"
    "src/wal_parser.cpp"
)

# Main GUI Executable
add_executable(wal_viewer_gui ${APP_SOURCES} ${IMGUI_SOURCES})

# Include directories
target_include_directories(wal_viewer_gui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    src
    ${stb_image_SOURCE_DIR}
)

# Link libraries
target_link_libraries(wal_viewer_gui glfw nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})

# PostgreSQL Headers
execute_process(
    COMMAND pg_config --includedir-server
    OUTPUT_VARIABLE PG_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND pg_config --includedir
    OUTPUT_VARIABLE PG_INCLUDE_CLIENT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND pg_config --libdir
    OUTPUT_VARIABLE PG_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "PostgreSQL Server Headers: ${PG_INCLUDE_DIR}")
message(STATUS "PostgreSQL Lib Dir: ${PG_LIB_DIR}")


if(PG_INCLUDE_DIR)
    target_include_directories(wal_viewer_gui PRIVATE ${PG_INCLUDE_DIR})
endif()

if(PG_INCLUDE_CLIENT_DIR)
    target_include_directories(wal_viewer_gui PRIVATE ${PG_INCLUDE_CLIENT_DIR})
endif()

if(PG_LIB_DIR)
    link_directories(${PG_LIB_DIR})
    target_link_libraries(wal_viewer_gui pq)
else()
    message(WARNING "Could not find PostgreSQL lib dir using pg_config")
endif()


# On Linux, we might need to link against GL and other system libs
if(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(wal_viewer_gui OpenGL::GL)
    target_link_libraries(wal_viewer_gui pthread)
endif()
