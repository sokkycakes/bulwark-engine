cmake_minimum_required(VERSION 3.16)
project(bulwark C CXX)

# Nintendo 3DS and DS builds use devkitPro's toolchain files
# (-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake or NDS.cmake); see
# docs/platforms/3ds.md and nds.md. They leave out what those systems can't host:
# dynamic plugins, PanoramaEngine and QuickJS, the host tools and tests.
# The original Xbox builds with nxdk's toolchain file
# (-DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake); see
# docs/platforms/xbox.md.
if(NINTENDO_3DS OR NINTENDO_DS OR NXDK)
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
if(NXDK)
	include(src/platform/xbox/xbox.cmake)
endif()

# The DS (4 MB of RAM, no FPU) can't host the engine: it gets a small Quake
# map viewer on its 3D hardware instead (docs/platforms/nds.md).
# BULWARK_NDS_NITROFS names a folder to pack into the ROM as nitro:/.
if(NINTENDO_DS)
	add_executable(bulwark_nds src/platform/nds/main_nds.c src/platform/nds/world_nds.c src/platform/nds/link_nds.c src/platform/nds/fs_nds.c
	               src/platform/nds/models_nds.c src/platform/nds/multipass_nds.c
	               src/platform/nds/portals_nds.c src/platform/nds/skel_nds.c src/platform/nds/pmove_nds.c
	               src/platform/nds/hull_nds.c src/platform/nds/weapon_nds.c src/platform/nds/hud_nds.c
	               src/platform/nds/events_nds.c src/platform/nds/sound_nds.c)
	target_link_libraries(bulwark_nds PRIVATE dswifi9 filesystem fat m)
	# sound runs a few hundred microseconds a frame: Thumb at -Os is about 11 KB smaller than
	# ARM at -O2, which the map load's lowest free RAM needs more (docs/platforms/nds.md "Sound")
	set_source_files_properties(src/platform/nds/sound_nds.c PROPERTIES COMPILE_OPTIONS "-Os;-mthumb")
	# libgcc's soft-float add, multiply/divide, compare and float-to-int run from ITCM (the
	# linker script puts *.itcm.* objects there): the movement prediction spends about half
	# its time in them. The same objects, so the same results, only faster fetched.
	execute_process(COMMAND ${CMAKE_C_COMPILER} -march=armv5te -mtune=arm946e-s -print-libgcc-file-name
	                OUTPUT_VARIABLE NDS_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
	set(NDS_SF_OBJS _arm_addsubsf3 _arm_muldivsf3 _arm_cmpsf2 _arm_fixsfsi _arm_unordsf2)
	string(REPLACE ";" " " NDS_SF_LIST "${NDS_SF_OBJS}")
	set(NDS_SF_OUT "")
	foreach(o ${NDS_SF_OBJS})
		list(APPEND NDS_SF_OUT ${CMAKE_BINARY_DIR}/sf_itcm/${o}.itcm.o)
	endforeach()
	file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/sf_itcm)
	add_custom_command(OUTPUT ${NDS_SF_OUT}
		COMMAND sh -c "for o in ${NDS_SF_LIST}; do ${CMAKE_AR} x ${NDS_LIBGCC} $o.o && mv $o.o $o.itcm.o; done"
		WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/sf_itcm VERBATIM)
	set_source_files_properties(${NDS_SF_OUT} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
	target_sources(bulwark_nds PRIVATE ${NDS_SF_OUT})
	set(BULWARK_NDS_NITROFS "" CACHE PATH "Folder to pack into the ROM (maps/, gfx/palette.lmp)")
	if(BULWARK_NDS_NITROFS)
		nds_create_rom(bulwark_nds NAME "Bulwark" SUBTITLE1 "Link client and map viewer" SUBTITLE2 "github.com/sokkycakes"
		               NITROFS "${BULWARK_NDS_NITROFS}")
	else()
		nds_create_rom(bulwark_nds NAME "Bulwark" SUBTITLE1 "Link client and map viewer" SUBTITLE2 "github.com/sokkycakes")
	endif()
	return()
endif()

if(NXDK)
	add_library(Threads::Threads INTERFACE IMPORTED) # src/platform/xbox/posix_xbox.c
else()
	find_package(Threads REQUIRED)
endif()

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
	src/net/pkg.c
	src/net/netchan.c)
target_link_libraries(bwnet PUBLIC bwcommon Threads::Threads ${CMAKE_DL_LIBS})

add_library(bwqcvm STATIC
	src/qcvm/qcvm.c
	src/qcvm/qc_exec.c
	src/qcvm/qc_jit.c
	src/qcvm/qc_strings.c
	src/qcvm/qc_scan.c
	src/qcvm/qc_builtins_core.c
	src/qcvm/qc_extra.c
	src/qcvm/qc_debug.c
	src/qcvm/qc_skel.c
	src/qcvm/qc_autocvar.c
	src/qcvm/qc_modelevents.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

if(NOT BULWARK_CONSOLE)
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
# the native C UI (src/ui/ui_native.c): Stiletto's HUD and menus drawn
# straight through the 2D API, for the Xbox where PanoramaEngine costs
# about 3.5 MB of XBE. The 3DS can opt in; the DS keeps hud_nds.c.
if(NXDK)
	set(bw_native_default ON)
else()
	set(bw_native_default OFF)
endif()
option(BULWARK_UI_NATIVE "Native C UI (Stiletto HUD and menus) instead of PanoramaEngine" ${bw_native_default})
if(BULWARK_UI_NATIVE AND BULWARK_UI_PANORAMA)
	# one ui.h backend per build, and QuickJS comes with PanoramaEngine
	set(BULWARK_UI_PANORAMA OFF)
	set(BULWARK_SCRIPT_JS OFF)
endif()
option(BULWARK_SCRIPT_JS "JavaScript scripting (needs BULWARK_UI_PANORAMA for QuickJS)" ${bw_desktop_default})
if(BULWARK_SCRIPT_JS AND NOT BULWARK_UI_PANORAMA)
	message(WARNING "BULWARK_SCRIPT_JS needs PanoramaEngine's QuickJS: turned off")
	set(BULWARK_SCRIPT_JS OFF)
endif()
if(BULWARK_SCRIPT_LUA)
	file(GLOB LUA_SOURCES third_party/lua-5.4.7/*.c)
	add_library(bwlua STATIC ${LUA_SOURCES})
	target_include_directories(bwlua PUBLIC third_party/lua-5.4.7)
	if(NXDK)
		# nxdk defines _WIN32, but Lua's Windows paths (popen, LoadLibrary) aren't there
		target_compile_options(bwlua PRIVATE -U_WIN32)
	endif()
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

if(NOT BULWARK_CONSOLE)
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
	# seeded random traces; compare two builds' output to prove a collision change exact
	add_executable(tracefuzz tests/bsp/tracefuzz.c)
	target_link_libraries(tracefuzz bwserver)
	add_test(NAME tracefuzz COMMAND sh -c "$<TARGET_FILE:tracefuzz> tests data 20000 maps/q1test.bsp maps/hltest.bsp > /dev/null"
	         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
	add_test(NAME bots COMMAND ${CMAKE_SOURCE_DIR}/tests/bots/run.sh $<TARGET_FILE:bulwark-ded>)
	add_test(NAME uri COMMAND ${CMAKE_SOURCE_DIR}/tests/uri/run.sh $<TARGET_FILE:bulwark-ded>)
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
if(NOT BULWARK_CONSOLE)
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
	if(NXDK)
		# quickjs-libc is the qjs shell's OS layer (files, timers, workers):
		# neither PanoramaEngine nor src/script uses it, and nxdk can't host it
		set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
	endif()
	add_subdirectory(third_party/PanoramaEngine EXCLUDE_FROM_ALL)
	if(NXDK)
		# as Lua: nxdk defines _WIN32, but QuickJS's Win32 code wants the
		# desktop API (threads, module paths, code pages). Its POSIX code
		# runs on the shims in src/platform/xbox/compat.
		target_compile_options(qjs PRIVATE -U_WIN32 -w "SHELL:-include sys/time.h")
		target_compile_definitions(qjs PRIVATE NO_TM_GMTOFF)
		# nxdk builds C++ without exceptions and has no C++ exception runtime.
		# Package reading and source cooking report errors by throwing: they
		# keep exceptions, and a throw stops the console (cxxrt_xbox.c).
		set_source_files_properties(
			third_party/PanoramaEngine/src/panorama_package.cpp
			third_party/PanoramaEngine/src/panorama_source_cooker.cpp
			third_party/PanoramaEngine/src/panorama_resource_provider.cpp
			TARGET_DIRECTORY PanoramaEngine
			PROPERTIES COMPILE_OPTIONS "-fexceptions;-fcxx-exceptions")
		# nxdk has no dynamic TLS initialisation (___dyn_tls_init). The
		# engine runs its views on the main thread only and PanoramaEngine
		# starts no threads, so its per-thread statistics become globals.
		target_compile_definitions(PanoramaEngine PRIVATE thread_local=)
		# std::filesystem's library half and the C++ exception entry points
		target_link_libraries(PanoramaEngine PRIVATE bwxboxcxx)
	endif()
	add_library(bwui STATIC
		src/ui/ui_panorama.cpp
		src/ui/ui_qc.c)
	target_link_libraries(bwui PUBLIC bwserver PRIVATE PanoramaEngine)
elseif(BULWARK_UI_NATIVE)
	add_library(bwui STATIC
		src/ui/ui_native.c
		src/ui/uin_draw.c
		src/ui/uin_atlas.c
		src/ui/uin_hud.c
		src/ui/uin_title.c
		src/ui/uin_lobby.c
		src/ui/uin_pause.c
		src/ui/uin_hero.c
		src/ui/uin_misc.c
		src/ui/ui_qc.c)
	target_link_libraries(bwui PUBLIC bwserver)
	# PUBLIC: the client (cl_input.c, cl_lobby.c, later cl_menu.c and
	# cl_emenu.c) links bwui and sees it too
	target_compile_definitions(bwui PUBLIC BW_UI_NATIVE=1)
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
	src/client/cl_webcore.c
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
elseif(NXDK)
	bulwark_xbox_executable()
elseif(SDL2_FOUND)
	add_executable(bulwark src/main.c src/client/vid_sdl.c src/sound/snd_sdl.c)
	target_include_directories(bulwark PRIVATE ${SDL2_INCLUDE_DIRS})
	target_link_libraries(bulwark bwclient ${SDL2_LIBRARIES})
else()
	message(WARNING "SDL2 not found: building the client without a window")
	add_executable(bulwark src/main.c src/client/vid_null.c src/sound/snd_null.c)
	target_link_libraries(bulwark bwclient)
endif()

# Source file formats (VTF, VMT, MDL) as a plugin, loaded at startup from
# next to the engine. Consoles can't load plugins.
if(NOT BULWARK_CONSOLE)
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
if(NOT BULWARK_CONSOLE)
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
	add_executable(gdidump tools/gdidump/gdidump.c)
	target_link_libraries(gdidump bwgdifont)
	add_executable(gdifonttest tests/gdifont/gdifonttest.c)
	target_link_libraries(gdifonttest bwgdifont)
	add_test(NAME gdifont COMMAND gdifonttest)
	add_test(NAME fontreload COMMAND ${CMAKE_SOURCE_DIR}/tests/fonts/run_font_tests.sh $<TARGET_FILE:bulwark>)
endif()
