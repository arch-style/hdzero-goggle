# Turns the translation files into a C source the app carries inside itself.
# Run at build time (cmake -P) with IN_DIR, the folder of <code>.ini files,
# and OUT, the .c to write. See language_init() for why.
file(GLOB INI_FILES "${IN_DIR}/*.ini")
list(SORT INI_FILES)

set(body "// Generated from ${IN_DIR} by src/lang/embed_languages.cmake. Do not edit.\n#include <stddef.h>\n\n")
set(table "")
foreach(ini ${INI_FILES})
    get_filename_component(code "${ini}" NAME_WE)
    file(READ "${ini}" hex HEX)
    string(LENGTH "${hex}" hex_len)
    math(EXPR size "${hex_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(APPEND body "static const unsigned char lang_${code}[] = {${bytes}};\n")
    string(APPEND table "    {\"${code}\", lang_${code}, ${size}},\n")
endforeach()

string(APPEND body "\nconst struct {\n    const char *code;\n    const unsigned char *data;\n    size_t size;\n} language_builtin[] = {\n${table}    {NULL, NULL, 0},\n};\n")
file(WRITE "${OUT}.tmp" "${body}")
configure_file("${OUT}.tmp" "${OUT}" COPYONLY)
