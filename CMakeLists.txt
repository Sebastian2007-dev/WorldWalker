cmake_minimum_required(VERSION 3.25)
project(WorldWalker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Vulkan REQUIRED COMPONENTS glslc)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW ON)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1 GIT_SHALLOW ON)

FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG 1.4.304 GIT_SHALLOW ON)
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)

FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.2.1 GIT_SHALLOW ON)

FetchContent_Declare(libdeflate GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git GIT_TAG v1.23 GIT_SHALLOW ON)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)

FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3 GIT_SHALLOW ON)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW ON)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.8 GIT_SHALLOW ON)

FetchContent_MakeAvailable(glfw glm volk vma libdeflate json stb imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC glfw volk)

file(GLOB_RECURSE WW_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_executable(WorldWalker ${WW_SOURCES})
target_include_directories(WorldWalker PRIVATE src ${stb_SOURCE_DIR})
target_compile_definitions(WorldWalker PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL
    NOMINMAX WIN32_LEAN_AND_MEAN VK_NO_PROTOTYPES
    WW_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
target_link_libraries(WorldWalker PRIVATE glfw glm::glm volk GPUOpen::VulkanMemoryAllocator
    libdeflate::libdeflate_static nlohmann_json::nlohmann_json imgui winhttp)
if(MSVC)
    target_compile_options(WorldWalker PRIVATE /W3 /MP /utf-8)
endif()

# ---- shaders -> bin/shaders/*.spv
file(GLOB WW_SHADERS CONFIGURE_DEPENDS shaders/*.vert shaders/*.frag shaders/*.comp)
file(GLOB WW_SHADER_INCLUDES CONFIGURE_DEPENDS shaders/*.glsl)
set(SPV_DIR ${CMAKE_BINARY_DIR}/bin/shaders)
set(SPV_FILES)
foreach(src ${WW_SHADERS})
    get_filename_component(name ${src} NAME)
    set(out ${SPV_DIR}/${name}.spv)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_DIR}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -I ${CMAKE_SOURCE_DIR}/shaders ${src} -o ${out}
        DEPENDS ${src} ${WW_SHADER_INCLUDES} VERBATIM)
    list(APPEND SPV_FILES ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SPV_FILES})
add_dependencies(WorldWalker shaders)

option(WW_BUILD_TESTS "Build character morphing and persistence integration tests" OFF)
if(WW_BUILD_TESTS)
    enable_testing()
    add_executable(HumanTests tools/tests/HumanTests.cpp src/render/Human.cpp src/render/Character.cpp
        src/gfx/Device.cpp src/gfx/Pipeline.cpp src/core/StbImpl.cpp)
    get_target_property(WW_INCLUDES WorldWalker INCLUDE_DIRECTORIES)
    get_target_property(WW_DEFINITIONS WorldWalker COMPILE_DEFINITIONS)
    get_target_property(WW_LIBRARIES WorldWalker LINK_LIBRARIES)
    target_include_directories(HumanTests PRIVATE ${WW_INCLUDES})
    target_compile_definitions(HumanTests PRIVATE ${WW_DEFINITIONS})
    target_link_libraries(HumanTests PRIVATE ${WW_LIBRARIES})
    add_dependencies(HumanTests shaders)
    add_test(NAME human-integration COMMAND HumanTests)
endif()
