# Turns a binary file into a C array: cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR size "${len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" hex "${hex}")
file(WRITE "${OUT}" "/* generated from ${IN} */\n#include <stddef.h>\nconst unsigned char ${NAME}[] = {\n${hex}\n};\nconst size_t ${NAME}_size = ${size};\n")
