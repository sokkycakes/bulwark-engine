cmake_minimum_required(VERSION 3.16)
project(bulwark C)

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
	src/common/pmove.c
	src/common/skel.c
	src/common/studio.c
	src/common/keyvalues.c
	src/common/vgui_res.c
	src/common/infoblob.c
	src/common/voxel.c
	src/common/stb_image_impl.c)
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
	src/qcvm/qc_extra.c
	src/qcvm/qc_skel.c
	src/qcvm/qc_autocvar.c
	src/qcvm/qc_modelevents.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

add_executable(qcrun tools/qcrun.c)
target_link_libraries(qcrun bwqcvm)

add_library(bwserver STATIC
	src/server/sv_main.c
	src/server/sv_world.c
	src/server/sv_builtins.c
	src/server/sv_client.c
	src/server/sv_pmove.c
	src/server/sv_route.c
	src/server/sv_voxel.c
	src/server/bspfile.c)
target_link_libraries(bwserver PUBLIC bwqcvm bwnet)

add_executable(bulwark-ded src/main_ded.c)
target_link_libraries(bulwark-ded bwserver)

add_executable(mdlcheck tools/mdlcheck.c)
target_link_libraries(mdlcheck bwcommon)
add_executable(studiotest tools/studiotest.c)
target_link_libraries(studiotest bwcommon)
add_executable(wavcheck tools/wavcheck.c)
target_link_libraries(wavcheck bwcommon)
add_executable(rescheck tools/rescheck.c)
target_link_libraries(rescheck bwcommon)
add_executable(vguitest tools/vguitest.c)
target_link_libraries(vguitest bwcommon)

enable_testing()
add_test(NAME qcvm COMMAND ${CMAKE_SOURCE_DIR}/tests/run_qc_tests.sh $<TARGET_FILE:qcrun>)
add_test(NAME studio COMMAND studiotest)
add_executable(voxeltest tools/voxeltest.c)
target_link_libraries(voxeltest bwcommon)
add_test(NAME voxel COMMAND voxeltest)
# real content, if BULWARK_STUDIO_FIXTURES names a folder of models (never committed)
add_test(NAME studio-fixtures COMMAND ${CMAKE_SOURCE_DIR}/tests/run_studio_fixtures.sh $<TARGET_FILE:mdlcheck>)
add_test(NAME vgui COMMAND vguitest)
# real .res files, if BULWARK_RES_FIXTURES names a folder of them (never committed)
add_test(NAME vgui-fixtures COMMAND ${CMAKE_SOURCE_DIR}/tests/run_res_fixtures.sh $<TARGET_FILE:rescheck>)
add_executable(bsptest tests/bsp/bsptest.c)
target_link_libraries(bsptest bwserver)
add_test(NAME bsp COMMAND bsptest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME bots COMMAND ${CMAKE_SOURCE_DIR}/tests/bots/run.sh $<TARGET_FILE:bulwark-ded>)

# game client with a listen server
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(SDL2 sdl2)
	endif()
endif()

add_library(bwclient STATIC
	src/client/cl_main.c
	src/client/cl_input.c
	src/client/cl_csqc.c
	src/client/cl_menu.c
	src/client/console.c
	src/render/r_main.c
	src/render/r_gl.c
	src/render/r_image.c
	src/render/r_material.c
	src/render/r_world.c
	src/render/r_q1world.c
	src/render/r_decal.c
	src/render/r_part.c
	src/render/r_draw.c
	src/render/r_model.c
	src/render/r_scene.c
	src/render/r_voxel.c
	src/sound/snd_main.c
	src/sound/snd_dsp.c
	src/sound/snd_mem.c)
target_link_libraries(bwclient PUBLIC bwserver)

if(SDL2_FOUND)
	add_executable(bulwark src/main.c src/client/vid_sdl.c src/sound/snd_sdl.c)
	target_include_directories(bulwark PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_libraries(bulwark bwclient ${SDL2_LIBRARIES})
else()
	message(WARNING "SDL2 not found: building the client without a window")
	add_executable(bulwark src/main.c src/client/vid_null.c src/sound/snd_null.c)
	target_link_libraries(bulwark bwclient)
endif()
