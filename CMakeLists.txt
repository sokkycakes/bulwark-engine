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
	src/common/studio.c
	src/common/keyvalues.c
	src/common/vgui_res.c
	src/common/infoblob.c
	src/common/voxel.c
	src/common/vbsp.c
	src/thirdparty/lzma/LzmaDec.c
	src/common/worker.c
	src/common/plugin.c
	src/common/terrain.c
	src/common/stb_image_impl.c
	src/common/json.c
	src/common/ftew.c)
target_link_libraries(bwcommon PUBLIC m Threads::Threads ${CMAKE_DL_LIBS})

add_library(bwscene STATIC
	src/scene/scene.c
	src/scene/scene_prefab.c
	src/scene/scene_ents.c)
target_link_libraries(bwscene PUBLIC bwcommon)

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
	src/qcvm/qc_autocvar.c
	src/qcvm/qc_modelevents.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

add_executable(qcrun tools/qcrun.c)
target_link_libraries(qcrun bwqcvm)

# scripting: Lua 5.4 (MIT, vendored in third_party) and QuickJS (MIT, from
# PanoramaEngine's Thirdparty, added below) behind src/script
option(BULWARK_SCRIPT_LUA "Lua scripting" ON)
option(BULWARK_SCRIPT_JS "JavaScript scripting" ON)
if(BULWARK_SCRIPT_LUA)
	file(GLOB LUA_SOURCES third_party/lua-5.4.7/*.c)
	add_library(bwlua STATIC ${LUA_SOURCES})
	target_include_directories(bwlua PUBLIC third_party/lua-5.4.7)
	if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(bwlua PRIVATE -w)
	endif()
	target_link_libraries(bwlua PUBLIC m)
endif()

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
	src/server/sv_voxel.c
	src/server/bspfile.c
	src/server/bsp_source.c
	src/server/bsp_props.c
	src/script/script.c
	src/script/script_api.c
	src/script/script_lua.c
	src/script/script_js.c
	src/script/script_qc.c)
target_link_libraries(bwserver PUBLIC bwqcvm bwnet bwscene)
if(BULWARK_SCRIPT_LUA)
	target_compile_definitions(bwserver PRIVATE BW_SCRIPT_LUA)
	target_link_libraries(bwserver PRIVATE bwlua)
endif()
if(BULWARK_SCRIPT_JS)
	target_compile_definitions(bwserver PRIVATE BW_SCRIPT_JS)
	target_link_libraries(bwserver PRIVATE qjs)
endif()

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
add_executable(bwscene-tool tools/bwscene.c src/server/bspfile.c src/server/bsp_source.c src/server/bsp_props.c)
set_target_properties(bwscene-tool PROPERTIES OUTPUT_NAME bwscene)
target_link_libraries(bwscene-tool bwscene)

add_executable(scenetest tests/scene/scenetest.c)
target_link_libraries(scenetest bwscene)

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
add_test(NAME scene COMMAND scenetest ${CMAKE_SOURCE_DIR}/tests/scene)

add_executable(ftewtest tests/ftew/ftewtest.c src/server/bspfile.c src/server/bsp_source.c src/server/bsp_props.c)
target_link_libraries(ftewtest bwcommon)
add_test(NAME ftew COMMAND ftewtest ${CMAKE_SOURCE_DIR}/tests/ftew)
set(BULWARK_NUCLIDE_BASE "" CACHE PATH "Nuclide's base/ directory, for the script tests")
if(BULWARK_NUCLIDE_BASE)
	add_test(NAME script COMMAND ${CMAKE_SOURCE_DIR}/tests/run_script_tests.sh $<TARGET_FILE:bulwark-ded>
	         ${BULWARK_NUCLIDE_BASE})
endif()

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
# bundled stand-ins for Tahoma, Tahoma Bold and Verdana (DejaVu Sans Condensed, its bold, DejaVu Sans; see data/fonts)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/bw_deffont.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed.ttf
		-DOUT=${CMAKE_BINARY_DIR}/generated/bw_deffont.c -DNAME=bw_deffont -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed-Bold.ttf
		-DOUT=${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c -DNAME=bw_deffont_bold -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSansCondensed-Bold.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/bw_widefont.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSans.ttf
		-DOUT=${CMAKE_BINARY_DIR}/generated/bw_widefont.c -DNAME=bw_widefont -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/data/fonts/DejaVuSans.ttf ${CMAKE_SOURCE_DIR}/cmake/embed.cmake)
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
	src/render/r_srcworld.c
	src/render/r_debug.c
	src/render/r_decal.c
	src/render/r_part.c
	src/render/r_tools.c
	src/render/r_rtlights.c
	src/render/r_viewer.c
	src/render/r_hud.c
	src/render/r_draw.c
	src/render/r_model.c
	src/render/r_scene.c
	src/render/r_voxel.c
	src/sound/snd_main.c
	src/sound/snd_dsp.c
	src/sound/snd_mem.c
	${CMAKE_BINARY_DIR}/generated/bw_deffont.c
	${CMAKE_BINARY_DIR}/generated/bw_deffont_bold.c
	${CMAKE_BINARY_DIR}/generated/bw_widefont.c)
target_link_libraries(bwclient PUBLIC bwserver bwui)

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
