file(GLOB_RECURSE GFX_SRCS "src/*.cpp" "src/*.c")
idf_component_register(SRCS ${GFX_SRCS} INCLUDE_DIRS "src" REQUIRES arduino-esp32 esp_lcd)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
