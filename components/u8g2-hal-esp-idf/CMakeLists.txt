if(CONFIG_DISPLAY_ENABLED)
    file(GLOB_RECURSE SOURCES src/*.c)
    idf_component_register(SRCS ${SOURCES}
                           INCLUDE_DIRS "include"
                           REQUIRES driver freertos u8g2)
else()
    idf_component_register(INCLUDE_DIRS "include")
endif()
