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
	src/common/json.c)
target_link_libraries(bwcommon PUBLIC m)

add_library(bwscene STATIC
	src/scene/scene.c
	src/scene/scene_prefab.c
	src/scene/scene_ents.c)
target_link_libraries(bwscene PUBLIC bwcommon)

add_library(bwqcvm STATIC
	src/qcvm/qcvm.c
	src/qcvm/qc_exec.c
	src/qcvm/qc_strings.c
	src/qcvm/qc_builtins_core.c
	src/qcvm/qc_extra.c)
target_link_libraries(bwqcvm PUBLIC bwcommon)

add_executable(qcrun tools/qcrun.c)
target_link_libraries(qcrun bwqcvm)

add_executable(bulwark-ded
	src/server/sv_main.c
	src/server/sv_world.c
	src/server/sv_builtins.c
	src/server/bspfile.c
	src/main_ded.c)
target_link_libraries(bulwark-ded bwqcvm bwscene)

add_executable(bwscene-tool tools/bwscene.c src/server/bspfile.c)
set_target_properties(bwscene-tool PROPERTIES OUTPUT_NAME bwscene)
target_link_libraries(bwscene-tool bwscene)

add_executable(scenetest tests/scene/scenetest.c)
target_link_libraries(scenetest bwscene)

enable_testing()
add_test(NAME qcvm COMMAND ${CMAKE_SOURCE_DIR}/tests/run_qc_tests.sh $<TARGET_FILE:qcrun>)
add_test(NAME scene COMMAND scenetest ${CMAKE_SOURCE_DIR}/tests/scene)
