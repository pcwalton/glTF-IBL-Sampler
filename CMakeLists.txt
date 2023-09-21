cmake_minimum_required(VERSION 3.12.0 FATAL_ERROR)

macro(add_sources expression sources)
    file(GLOB source_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${expression}")
    set(${sources} "${${sources}};${source_files}")
endmacro()

macro(add_include_folder include_folder includes)
    set(${includes} "${${includes}};${CMAKE_CURRENT_SOURCE_DIR}/${include_folder}")
endmacro()

function(cmake_option option description default)
    option(${option} description ${default})
    if(${option})
        message(STATUS "'${option}' is TRUE")
    else()
        message(STATUS "'${option}' is FALSE")
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")


set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Linux")
    # use, i.e. don't skip the full RPATH for the build tree
    set(CMAKE_SKIP_BUILD_RPATH FALSE)

    # when building, don't use the install RPATH already
    # (but later on when installing)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

    # the RPATH to be used when installing
    set(CMAKE_INSTALL_RPATH "\$ORIGIN")
    #set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX_ABSOLUTE_PATH}/usr/lib")

    # add the automatically determined parts of the RPATH
    # which point to directories outside the build tree to the install RPATH
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

    set(CMAKE_SKIP_RPATH FALSE)
endif()


#this project
project(glTFIBLSampler)

cmake_option(IBLSAMPLER_EXPORT_SHADERS "" OFF)

set(IBLSAMPLER_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/shaders" CACHE STRING "")

if (IBLSAMPLER_EXPORT_SHADERS)
    set(IBLSAMPLER_SHADERS_DIR "shaders" CACHE INTERNAL "" FORCE)
endif()

add_definitions(-DIBLSAMPLER_SHADERS_DIR="${IBLSAMPLER_SHADERS_DIR}")

if (IBLSAMPLER_EXPORT_SHADERS)
    if (WIN32)
        file(COPY "lib/shaders" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release/")
        file(COPY "lib/shaders" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug/")
    else()
        file(COPY "lib/shaders" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/")
    endif()
endif()

#lib sources
add_sources("lib/source/*.cpp" lib_sources)
add_sources("lib/source/*.h" lib_sources)
add_sources("${CMAKE_CURRENT_SOURCE_DIR}/lib/include/*.h" lib_headers)
add_include_folder("lib/include" lib_include_dirs)

# STB
set(STB_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/stb/" CACHE STRING "")
set(lib_include_dirs "${lib_include_dirs};${STB_INCLUDE_PATH}")

#lib project
add_library(GltfIblSampler SHARED ${lib_sources} ${lib_headers})
target_include_directories(GltfIblSampler PUBLIC "${lib_include_dirs}")
target_include_directories(GltfIblSampler PRIVATE "thirdparty/Vulkan-Headers/include")
# specify the public headers (will be copied to `include` in install step)
set_target_properties(GltfIblSampler PROPERTIES PUBLIC_HEADER "${lib_headers}")

# Volk
set(VULKAN_HEADERS_INSTALL_DIR "../Vulkan-Headers")
add_subdirectory("thirdparty/volk")
target_link_libraries(GltfIblSampler PRIVATE volk)

#cli project
add_sources("cli/source/*.cpp" "cli_sources")
add_executable(cli "${cli_sources}")
target_link_libraries(cli PUBLIC GltfIblSampler)
target_link_libraries(cli PRIVATE volk)

message(STATUS "")
install(TARGETS cli GltfIblSampler)
