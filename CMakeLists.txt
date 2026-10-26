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
	src/common/cvar.c
	src/common/cmd.c
	src/common/fs.c)
target_link_libraries(bwcommon PUBLIC m)

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
target_link_libraries(bulwark-ded bwqcvm)

enable_testing()
add_test(NAME qcvm COMMAND ${CMAKE_SOURCE_DIR}/tests/run_qc_tests.sh $<TARGET_FILE:qcrun>)
