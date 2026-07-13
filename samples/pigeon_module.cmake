# Shared by every sample under samples/: pulls in the pigeon module from its
# sibling checkout so a plain `west build` (and clangd, via the generated
# compile_commands.json) picks it up without needing extra -D flags.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/../pigeon)
