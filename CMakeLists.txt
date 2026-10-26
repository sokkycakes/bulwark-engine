cmake_minimum_required(VERSION 3.16)
project(bulwark C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
	add_compile_definitions(_GNU_SOURCE)
endif()

add_library(bwcommon STATIC
	src/common/common.c
	src/common/digest.c
	src/common/cvar.c
	src/common/cmd.c
	src/common/fs.c
	src/common/pmove.c)
target_link_libraries(bwcommon PUBLIC m)

add_library(bwnet STATIC
	src/net/msg.c
	src/net/net.c
	src/net/netchan.c)
target_link_libraries(bwnet PUBLIC bwcommon)

add_library(bwqcvm STATIC
	src/qcvm/qcvm.c
	src/qcvm/qc_exec.c
	src/qcvm/qc_strings.c
	src/qcvm/qc_builtins_core.c
	src/qcvm/qc_extra.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

add_executable(qcrun tools/qcrun.c)
target_link_libraries(qcrun bwqcvm)

add_library(bwserver STATIC
	src/server/sv_main.c
	src/server/sv_world.c
	src/server/sv_builtins.c
	src/server/sv_client.c
	src/server/sv_pmove.c
	src/server/bspfile.c)
target_link_libraries(bwserver PUBLIC bwqcvm bwnet)

add_executable(bulwark-ded src/main_ded.c)
target_link_libraries(bulwark-ded bwserver)

enable_testing()
add_test(NAME qcvm COMMAND ${CMAKE_SOURCE_DIR}/tests/run_qc_tests.sh $<TARGET_FILE:qcrun>)

# game client with a listen server
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(SDL2 sdl2)
	endif()
endif()

# UI: PanoramaEngine (MIT, vendored in third_party) behind src/ui
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PANORAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PANORAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PANORAMA_BUILD_PERFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/PanoramaEngine EXCLUDE_FROM_ALL)
add_library(bwui STATIC
	src/ui/ui_panorama.cpp
	src/ui/ui_qc.c)
target_link_libraries(bwui PUBLIC bwserver PRIVATE PanoramaEngine)

add_library(bwclient STATIC
	src/client/cl_main.c
	src/client/cl_input.c
	src/client/cl_csqc.c
	src/client/console.c
	src/render/r_main.c
	src/render/r_gl.c
	src/render/r_image.c
	src/render/r_material.c
	src/render/r_world.c
	src/render/r_draw.c
	src/render/r_model.c
	src/render/r_scene.c
	src/sound/snd_main.c)
target_link_libraries(bwclient PUBLIC bwserver bwui)

if(SDL2_FOUND)
	add_executable(bulwark src/main.c src/client/vid_sdl.c)
	target_include_directories(bulwark PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_libraries(bulwark bwclient ${SDL2_LIBRARIES})
else()
	message(WARNING "SDL2 not found: building the client without a window")
	add_executable(bulwark src/main.c src/client/vid_null.c)
	target_link_libraries(bulwark bwclient)
endif()
