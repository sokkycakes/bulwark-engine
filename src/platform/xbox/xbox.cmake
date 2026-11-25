# Bulwark engine - the original Xbox build, through nxdk. ISC licence, see LICENSE.
#
#   export NXDK_DIR=/path/to/nxdk
#   cmake -S . -B build-xbox -DCMAKE_TOOLCHAIN_FILE=$NXDK_DIR/share/toolchain-nxdk.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-xbox --target bulwark_xbe
#
# This file is included from the top-level CMakeLists.txt when NXDK is set.
# It turns the console defaults on (no plugins, no PanoramaEngine, no host
# tools), points the compiler at the POSIX shims in compat/, and defines
# bulwark_xbox_executable(), which builds bulwark.exe, default.xbe and an
# ISO image for xemu. docs/platforms/xbox.md has the details.

set(NXDK_DIR $ENV{NXDK_DIR})
set(BW_XBOX_DIR ${CMAKE_SOURCE_DIR}/src/platform/xbox)

# the Pentium III has SSE but not SSE2: the image and mp3 decoders' SIMD paths are out
add_compile_definitions(BW_XBOX=1 XBOX=1 STBI_NO_SIMD MINIMP3_NO_SIMD)
# POSIX headers the engine includes and nxdk lacks; first on the path
include_directories(BEFORE ${BW_XBOX_DIR}/compat)
# lwIP's BSD socket API (nxdk's net library) and SDL2
include_directories(${NXDK_DIR}/lib/net/lwip/src/include ${NXDK_DIR}/lib/net/nforceif/include
                    ${NXDK_DIR}/lib/net/nvnetdrv ${NXDK_DIR}/lib/sdl/SDL2/include)
# path translation and the POSIX names, in every C file
add_compile_options("SHELL:-include ${BW_XBOX_DIR}/compat/bw_xbox.h")
# nxdk-cc passes -fno-builtin; give memcpy, sqrtf and friends back to the
# optimiser. The Pentium III has SSE (single precision): use it for floats.
add_compile_options(-fbuiltin -mfpmath=sse -Wno-unused-function)

set(BW_XBOX_LIBS
	${NXDK_DIR}/lib/libSDL2.lib
	${NXDK_DIR}/lib/libpbkit.lib
	${NXDK_DIR}/lib/libnxdk_net.lib
	${NXDK_DIR}/lib/libnxdk_automount_d.lib
	${NXDK_DIR}/lib/libzlib.lib)

function(bulwark_xbox_executable)
	add_executable(bulwark src/main.c
		${BW_XBOX_DIR}/sys_xbox.c
		${BW_XBOX_DIR}/vid_xbox.c
		${BW_XBOX_DIR}/gl_xbox.c
		${BW_XBOX_DIR}/snd_xbox.c)
	target_link_libraries(bulwark bwclient bwxboxposix ${BW_XBOX_LIBS})
	# the main thread's stack (nxdk's default is 64 KB; the engine keeps
	# several 8 KB buffers on it) and D: mounted on the XBE's folder
	target_link_options(bulwark PRIVATE -stack:1048576 -include:_automount_d_drive -map:${CMAKE_BINARY_DIR}/bulwark.map)
	set(xbe ${CMAKE_BINARY_DIR}/xbe/default.xbe)
	add_custom_command(OUTPUT ${xbe}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/xbe
		COMMAND ${NXDK_DIR}/tools/cxbe/cxbe -OUT:${xbe} -TITLE:Bulwark $<TARGET_FILE:bulwark>
		DEPENDS bulwark
		COMMENT "default.xbe")
	add_custom_target(bulwark_xbe ALL DEPENDS ${xbe})
endfunction()

add_library(bwxboxposix STATIC ${BW_XBOX_DIR}/posix_xbox.c ${BW_XBOX_DIR}/fmt_xbox.c ${BW_XBOX_DIR}/math_xbox.c)

# PDCLib's maths live in the C library: "m" links to nothing
add_library(m INTERFACE)
