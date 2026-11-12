# Wraps glslc -mfmt=c output ({0x...,...}) in a C array definition.
file(READ "${IN}" body)
file(WRITE "${OUT}" "/* generated from glslc output; do not edit */\nstatic const uint32_t ${NAME}[] =\n${body};\n")
