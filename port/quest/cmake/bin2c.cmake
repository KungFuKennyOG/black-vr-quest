# Wraps a binary file in a C array: cmake -DIN=<file> -DOUT=<inc> -DNAME=<symbol> -P bin2c.cmake
#   static const unsigned char NAME[] = { ... }; static const unsigned int NAME_len = N;
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" body "${body}")
file(WRITE "${OUT}" "/* generated from ${IN}; do not edit */\nstatic const unsigned char ${NAME}[] = {\n${body}\n};\nstatic const unsigned int ${NAME}_len = ${bytes};\n")
