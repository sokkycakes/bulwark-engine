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

find_package(Threads REQUIRED)

add_library(bwcommon STATIC
	src/common/common.c
	src/common/digest.c
	src/common/cvar.c
	src/common/cmd.c
	src/common/cmd_extra.c
	src/common/cmd_compat.c
	src/common/bsptool.c
	src/common/fs.c
	src/common/manifest.c
	src/common/pmove.c
	src/common/skel.c
	src/common/infoblob.c
	src/common/worker.c
	src/common/plugin.c
	src/common/terrain.c
	src/common/stb_image_impl.c)
target_link_libraries(bwcommon PUBLIC m Threads::Threads ${CMAKE_DL_LIBS})

add_library(bwnet STATIC
	src/net/msg.c
	src/net/net.c
	src/net/http.c
	src/net/pkg.c
	src/net/netchan.c)
target_link_libraries(bwnet PUBLIC bwcommon Threads::Threads ${CMAKE_DL_LIBS})

add_library(bwqcvm STATIC
	src/qcvm/qcvm.c
	src/qcvm/qc_exec.c
	src/qcvm/qc_strings.c
	src/qcvm/qc_builtins_core.c
	src/qcvm/qc_extra.c
	src/qcvm/qc_debug.c
	src/qcvm/qc_skel.c
	src/qcvm/qc_autocvar.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

add_executable(qcrun tools/qcrun.c)
target_link_libraries(qcrun bwqcvm)

add_library(bwserver STATIC
	src/server/sv_main.c
	src/server/sv_compile.c
	src/server/sv_world.c
	src/server/sv_builtins.c
	src/server/sv_client.c
	src/server/sv_pmove.c
	src/server/sv_route.c
	src/server/sv_cmds.c
	src/server/sv_save.c
	src/server/sv_compat.c
	src/server/sv_admin.c
	src/server/sv_mvd.c
	src/server/sv_master.c
	src/server/sv_download.c
	src/server/sv_voip.c
	src/server/sv_sql.c
	src/server/sv_stats.c
	src/server/bspfile.c)
target_link_libraries(bwserver PUBLIC bwqcvm bwnet)

add_executable(bulwark-ded src/main_ded.c)
target_link_libraries(bulwark-ded bwserver)

enable_testing()
add_test(NAME qcvm COMMAND ${CMAKE_SOURCE_DIR}/tests/run_qc_tests.sh $<TARGET_FILE:qcrun>)
add_executable(bsptest tests/bsp/bsptest.c)
target_link_libraries(bsptest bwserver)
add_test(NAME bsp COMMAND bsptest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# game client with a listen server
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(SDL2 sdl2)
	endif()
endif()

# the bundled fallbacks for the default font (DejaVu Sans Condensed and its bold, see data/fonts)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/bw_deffont.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed.ttf
		-DOUT=${CMAKE_BINARY_DIR}/generated/bw_deffont.c -DNAME=bw_deffont -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed-Bold.ttf
		-DOUT=${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c -DNAME=bw_deffont_bold -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed-Bold.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)

add_library(bwclient STATIC
	src/client/cl_master.c
	src/client/cl_download.c
	src/client/cl_capture.c
	src/client/cl_cinematic.c
	src/client/cl_voip.c
	src/client/cl_emenu.c
	src/client/cl_editor.c
	src/client/cl_main.c
	src/client/cl_input.c
	src/client/cl_csqc.c
	src/client/cl_menu.c
	src/client/console.c
	src/client/cl_cmds.c
	src/client/cl_chat.c
	src/client/cl_tp.c
	src/client/cl_demo.c
	src/render/r_main.c
	src/render/r_overlay.c
	src/render/r_gl.c
	src/render/r_image.c
	src/render/r_material.c
	src/render/r_world.c
	src/render/r_q1world.c
	src/render/r_decal.c
	src/render/r_part.c
	src/render/r_tools.c
	src/render/r_rtlights.c
	src/render/r_viewer.c
	src/render/r_hud.c
	src/render/r_draw.c
	src/render/r_model.c
	src/render/r_scene.c
	src/sound/snd_main.c
	src/sound/snd_dsp.c
	src/sound/snd_mem.c
	${CMAKE_BINARY_DIR}/generated/bw_deffont.c
	${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c)
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

# Source file formats (VTF, VMT, MDL) as a plugin, loaded at startup from
# next to the engine
add_library(bwplug_source MODULE
	plugins/source/plugin.c
	plugins/source/vtf.c
	plugins/source/vmt.c
	plugins/source/mdl.c)
set_target_properties(bwplug_source PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
target_link_libraries(bwplug_source m)
add_executable(srcconv tools/srcconv.c)
target_link_libraries(srcconv ${CMAKE_DL_LIBS})
