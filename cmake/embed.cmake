# cmake -DINPUT=file -DOUTPUT=file.c -DSYMBOL=name -P embed.cmake
# Writes a C file with INPUT's bytes as `const unsigned char SYMBOL[]` and
# their count as `const size_t SYMBOL_SIZE`.
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}" "/* Generated from ${name} by cmake/embed.cmake: do not edit. */\n#include <stddef.h>\nconst unsigned char ${SYMBOL}[] = {\n${bytes}};\nconst size_t ${SYMBOL}_SIZE = sizeof ${SYMBOL};\n")
