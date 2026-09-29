set(XIAODEMO_TARGET "xiaodemo")

add_executable(${XIAODEMO_TARGET}
    src/main.c)

target_link_libraries(${XIAODEMO_TARGET}
    PRIVATE lvgl::lvgl
    PRIVATE lvgl::drivers
    PRIVATE pthread)

install(TARGETS ${XIAODEMO_TARGET}
    RUNTIME DESTINATION xiaodemo)
