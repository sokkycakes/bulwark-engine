cmake_minimum_required(VERSION 3.16)
project(bulwark C CXX)

# Nintendo 3DS and DS builds use devkitPro's toolchain files
# (-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake or NDS.cmake); see
# docs/platforms/3ds.md and nds.md. They leave out what those systems can't host:
# dynamic plugins, PanoramaEngine and QuickJS, the host tools and tests.
# The browser build uses Emscripten (emcmake cmake ..., or tools/web/build.sh);
# see docs/platforms/web.md. Like the consoles it leaves out the host tools
# and tests, and it links its plugins in, having no dynamic linker.
if(EMSCRIPTEN)
	set(BULWARK_WEB ON)
else()
	set(BULWARK_WEB OFF)
endif()
if(NINTENDO_3DS OR NINTENDO_DS)
	set(BULWARK_CONSOLE ON)
	set(CMAKE_DL_LIBS "") # no dynamic linker (src/common/sys_dl.h)
else()
	set(BULWARK_CONSOLE OFF)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
	add_compile_definitions(_GNU_SOURCE)
endif()

# The DS (4 MB of RAM, no FPU) can't host the engine: it gets a small Quake
# map viewer on its 3D hardware instead (docs/platforms/nds.md).
# BULWARK_NDS_NITROFS names a folder to pack into the ROM as nitro:/.
if(NINTENDO_DS)
	add_executable(bulwark_nds src/platform/nds/main_nds.c src/platform/nds/world_nds.c src/platform/nds/link_nds.c src/platform/nds/fs_nds.c
	               src/platform/nds/models_nds.c src/platform/nds/multipass_nds.c
	               src/platform/nds/portals_nds.c src/platform/nds/skel_nds.c)
	target_link_libraries(bulwark_nds PRIVATE dswifi9 filesystem fat m)
	set(BULWARK_NDS_NITROFS "" CACHE PATH "Folder to pack into the ROM (maps/, gfx/palette.lmp)")
	if(BULWARK_NDS_NITROFS)
		nds_create_rom(bulwark_nds NAME "Bulwark" SUBTITLE1 "Link client and map viewer" SUBTITLE2 "github.com/sokkycakes"
		               NITROFS "${BULWARK_NDS_NITROFS}")
	else()
		nds_create_rom(bulwark_nds NAME "Bulwark" SUBTITLE1 "Link client and map viewer" SUBTITLE2 "github.com/sokkycakes")
	endif()
	return()
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
	src/common/bmfont.c
	src/common/ftew.c)
target_link_libraries(bwcommon PUBLIC m Threads::Threads ${CMAKE_DL_LIBS})
# Bulwark's own data files (UI layouts), found here when nothing is installed beside the executable
target_compile_definitions(bwcommon PRIVATE BW_SOURCE_DATADIR="${CMAKE_SOURCE_DIR}/data")

add_library(bwscene STATIC
	src/scene/scene.c
	src/scene/scene_prefab.c
	src/scene/scene_ents.c)
target_link_libraries(bwscene PUBLIC bwcommon)

add_library(bwnet STATIC
	src/net/msg.c
	src/net/net.c
	src/net/http.c
	src/net/live.c
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

if(NOT BULWARK_CONSOLE AND NOT BULWARK_WEB)
	add_executable(qcrun tools/qcrun.c)
	target_link_libraries(qcrun bwqcvm)
endif()

# scripting: Lua 5.4 (MIT, vendored in third_party) and QuickJS (MIT, from
# PanoramaEngine's Thirdparty, added below) behind src/script
option(BULWARK_SCRIPT_LUA "Lua scripting" ON)
if(BULWARK_CONSOLE)
	set(bw_desktop_default OFF)
else()
	set(bw_desktop_default ON)
endif()
option(BULWARK_UI_PANORAMA "Panorama UI views (PanoramaEngine, C++20)" ${bw_desktop_default})
option(BULWARK_SCRIPT_JS "JavaScript scripting (needs BULWARK_UI_PANORAMA for QuickJS)" ${bw_desktop_default})
if(BULWARK_SCRIPT_JS AND NOT BULWARK_UI_PANORAMA)
	message(WARNING "BULWARK_SCRIPT_JS needs PanoramaEngine's QuickJS: turned off")
	set(BULWARK_SCRIPT_JS OFF)
endif()
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
	src/server/sv_uri.c
	src/server/sv_builtins.c
	src/server/sv_client.c
	src/server/sv_link.c
	src/server/sv_pmove.c
	src/server/sv_route.c
	src/server/sv_cmds.c
	src/server/sv_save.c
	src/server/sv_compat.c
	src/server/sv_admin.c
	src/server/sv_livesets.c
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

if(NOT BULWARK_CONSOLE AND NOT BULWARK_WEB)
	add_executable(bulwark-ded src/main_ded.c)
	target_link_libraries(bulwark-ded bwserver)

	add_executable(mdlcheck tools/mdlcheck.c)
	target_link_libraries(mdlcheck bwcommon)
	add_executable(studiotest tools/studiotest.c)
	target_link_libraries(studiotest bwcommon)
	add_executable(wavcheck tools/wavcheck.c src/sound/snd_mp3.c)
	target_link_libraries(wavcheck bwcommon)
	add_executable(rescheck tools/rescheck.c)
	target_link_libraries(rescheck bwcommon)
	add_executable(bmfdump tools/bmfdump.c)
	target_link_libraries(bmfdump bwcommon)
	add_executable(bmfonttest tests/bmfont/bmfonttest.c)
	target_link_libraries(bmfonttest bwcommon)
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
	add_test(NAME bmfont COMMAND ${CMAKE_SOURCE_DIR}/tests/bmfont/run.sh $<TARGET_FILE:bmfonttest>)
	# real .res files, if BULWARK_RES_FIXTURES names a folder of them (never committed)
	add_test(NAME vgui-fixtures COMMAND ${CMAKE_SOURCE_DIR}/tests/run_res_fixtures.sh $<TARGET_FILE:rescheck>)
	add_executable(bsptest tests/bsp/bsptest.c)
	target_link_libraries(bsptest bwserver)
	add_test(NAME bsp COMMAND bsptest WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
	add_test(NAME bots COMMAND ${CMAKE_SOURCE_DIR}/tests/bots/run.sh $<TARGET_FILE:bulwark-ded>)
	add_test(NAME uri COMMAND ${CMAKE_SOURCE_DIR}/tests/uri/run.sh $<TARGET_FILE:bulwark-ded>)
	# the master server on its own; tests/serverbrowser/run.py is the end-to-end run (needs Stiletto and a display)
	find_program(BW_PYTHON3 python3)
	if(BW_PYTHON3)
		add_test(NAME serverbrowser-master COMMAND ${BW_PYTHON3} ${CMAKE_SOURCE_DIR}/tests/serverbrowser/master_test.py)
	endif()
	add_test(NAME scene COMMAND scenetest ${CMAKE_SOURCE_DIR}/tests/scene)

	add_executable(ftewtest tests/ftew/ftewtest.c src/server/bspfile.c src/server/bsp_source.c src/server/bsp_props.c)
	target_link_libraries(ftewtest bwcommon)
	add_test(NAME ftew COMMAND ftewtest ${CMAKE_SOURCE_DIR}/tests/ftew)
	set(BULWARK_NUCLIDE_BASE "" CACHE PATH "Nuclide's base/ directory, for the script tests")
	if(BULWARK_NUCLIDE_BASE)
		add_test(NAME script COMMAND ${CMAKE_SOURCE_DIR}/tests/run_script_tests.sh $<TARGET_FILE:bulwark-ded>
		         ${BULWARK_NUCLIDE_BASE})
	endif()
endif()

# game client with a listen server
if(BULWARK_WEB)
	# Emscripten's own SDL2 (its ports), over the browser's canvas, Web Audio
	# and Gamepad API
	set(SDL2_FOUND ON)
	set(SDL2_INCLUDE_DIRS "")
	set(SDL2_LIBRARIES "-sUSE_SDL=2")
	add_compile_options(-sUSE_SDL=2)
elseif(NOT BULWARK_CONSOLE)
	find_package(SDL2 QUIET)
endif()
if(NOT SDL2_FOUND AND NOT BULWARK_CONSOLE)
	find_package(PkgConfig QUIET)
	if(PkgConfig_FOUND)
		pkg_check_modules(SDL2 sdl2)
	endif()
endif()

# UI: PanoramaEngine (MIT, vendored in third_party) behind src/ui
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(BULWARK_UI_PANORAMA)
	set(PANORAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(PANORAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(PANORAMA_BUILD_PERFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
	add_subdirectory(third_party/PanoramaEngine EXCLUDE_FROM_ALL)
	add_library(bwui STATIC
		src/ui/ui_panorama.cpp
		src/ui/ui_qc.c)
	target_link_libraries(bwui PUBLIC bwserver PRIVATE PanoramaEngine)
else()
	add_library(bwui STATIC
		src/ui/ui_null.c
		src/ui/ui_qc.c)
	target_link_libraries(bwui PUBLIC bwserver)
endif()

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
	src/client/cl_altcon.c
	src/client/cl_lobby.c
	src/client/cl_dedlaunch.c
	src/client/cl_webcore.c
	src/client/cl_setflow.c
	src/client/cl_chat.c
	src/client/cl_tp.c
	src/client/cl_seat.c
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
	src/render/r_pcf.c
	src/render/r_tools.c
	src/render/r_rtlights.c
	src/render/r_skin.c
	src/render/r_svg.c
	src/render/r_viewer.c
	src/render/r_hud.c
	src/render/r_draw.c
	src/render/r_gdifont.c
	src/render/r_bmfont.c
	src/render/r_model.c
	src/render/r_scene.c
	src/render/r_voxel.c
	src/sound/snd_main.c
	src/sound/snd_dsp.c
	src/sound/snd_mem.c
	src/sound/snd_mp3.c)
target_link_libraries(bwclient PUBLIC bwserver bwui)
target_include_directories(bwclient PRIVATE third_party)

if(NINTENDO_3DS)
	# shaders for the GL subset on the PICA200 (src/platform/ctr/gl_ctr.c)
	# (the files go unnamed: this devkitPro release rejects its own SOURCES keyword)
	ctr_add_shader_library(bwctr_shaders
		src/platform/ctr/shaders/ctr_2d.v.pica
		src/platform/ctr/shaders/ctr_world.v.pica
		src/platform/ctr/shaders/ctr_model.v.pica
		src/platform/ctr/shaders/ctr_sky.v.pica)
	dkp_add_embedded_binary_library(bwctr_shaders_bin bwctr_shaders)
	add_executable(bulwark src/main.c
		src/platform/ctr/sys_ctr.c
		src/platform/ctr/vid_ctr.c
		src/platform/ctr/gl_ctr.c
		src/platform/ctr/snd_ctr.c)
	target_link_libraries(bulwark bwclient bwctr_shaders_bin citro3d)
	ctr_generate_smdh(bulwark.smdh
		NAME "Bulwark"
		DESCRIPTION "Bulwark engine"
		AUTHOR "sokky")
	ctr_create_3dsx(bulwark SMDH bulwark.smdh)
elseif(BULWARK_WEB)
	add_executable(bulwark src/main.c src/client/vid_sdl.c src/sound/snd_sdl.c src/platform/web/sys_web.c)
	target_link_libraries(bulwark bwclient ${SDL2_LIBRARIES})
	# the page around it is tools/web/shell; tools/web/export.py puts the two
	# and a game's files together into a folder any web server can host
	target_link_options(bulwark PRIVATE
		-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sGL_ENABLE_GET_PROC_ADDRESS=1
		-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=8MB
		-sFORCE_FILESYSTEM=1 -lidbfs.js -sFETCH=1
		-sEXIT_RUNTIME=0 -sINVOKE_RUN=0
		-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,callMain,ccall,cwrap,UTF8ToString,stringToNewUTF8,addRunDependency,removeRunDependency
		-sEXPORTED_FUNCTIONS=_main,_malloc,_free
		-sENVIRONMENT=web,node)
	set_target_properties(bulwark PROPERTIES SUFFIX ".js")
elseif(SDL2_FOUND)
	add_executable(bulwark src/main.c src/client/vid_sdl.c src/client/in_steam.c src/client/steaminput.c src/sound/snd_sdl.c)
	target_include_directories(bulwark PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_libraries(bulwark bwclient ${SDL2_LIBRARIES})
else()
	message(WARNING "SDL2 not found: building the client without a window")
	add_executable(bulwark src/main.c src/client/vid_null.c src/sound/snd_null.c)
	target_link_libraries(bulwark bwclient)
endif()

# Source file formats (VTF, VMT, MDL) as a plugin, loaded at startup from
# next to the engine. Consoles can't load plugins; the browser links it in.
if(BULWARK_WEB)
	add_library(bwplug_source STATIC
		plugins/source/plugin.c
		plugins/source/vtf.c
		plugins/source/vmt.c
		plugins/source/mdl.c
		plugins/source/mdlskel.c)
	# its entry point and its globals get names of their own inside the engine
	target_compile_definitions(bwplug_source PRIVATE bwplug_init=bwplug_source_init api=bwplug_source_api)
	target_compile_definitions(bwcommon PRIVATE BW_LINKED_PLUGIN_SOURCE)
	target_link_libraries(bulwark bwplug_source)
endif()
if(NOT BULWARK_CONSOLE AND NOT BULWARK_WEB)
add_library(bwplug_source MODULE
	plugins/source/plugin.c
	plugins/source/vtf.c
	plugins/source/vmt.c
	plugins/source/mdl.c
	plugins/source/mdlskel.c)
set_target_properties(bwplug_source PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
target_link_libraries(bwplug_source m)
add_executable(srcconv tools/srcconv.c)
target_link_libraries(srcconv ${CMAKE_DL_LIBS})
endif()

# GDI-compatible TrueType rasterizer (docs/spec/gdi-text.md). Optional: only
# built when FreeType is installed. With it, the renderer draws TrueType
# fonts through it (src/render/r_gdifont.c); without it, stb_truetype as before.
if(BULWARK_WEB)
	# Emscripten's FreeType port
	if(NOT TARGET Freetype::Freetype)
		add_library(Freetype::Freetype INTERFACE IMPORTED)
		set_target_properties(Freetype::Freetype PROPERTIES INTERFACE_COMPILE_OPTIONS "-sUSE_FREETYPE=1"
		                      INTERFACE_LINK_OPTIONS "-sUSE_FREETYPE=1")
	endif()
	set(FREETYPE_FOUND ON)
elseif(NOT BULWARK_CONSOLE)
	find_package(Freetype QUIET)
endif()
if(FREETYPE_FOUND)
	add_library(bwgdifont STATIC src/client/gdifont.c)
	target_link_libraries(bwgdifont PUBLIC Freetype::Freetype)
	target_link_libraries(bwclient PUBLIC bwgdifont)
	target_compile_definitions(bwclient PRIVATE BW_HAVE_GDIFONT)
	if(TARGET bwui AND BULWARK_UI_PANORAMA)
		target_link_libraries(bwui PRIVATE bwgdifont)
		target_compile_definitions(bwui PRIVATE BW_HAVE_GDIFONT)
	endif()
	if(NOT BULWARK_WEB)
		add_executable(gdidump tools/gdidump/gdidump.c)
		target_link_libraries(gdidump bwgdifont)
		add_executable(gdifonttest tests/gdifont/gdifonttest.c)
		target_link_libraries(gdifonttest bwgdifont)
		add_test(NAME gdifont COMMAND gdifonttest)
		add_test(NAME fontreload COMMAND ${CMAKE_SOURCE_DIR}/tests/fonts/run_font_tests.sh $<TARGET_FILE:bulwark>)
	endif()
endif()
