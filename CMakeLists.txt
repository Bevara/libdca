cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBDCA_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_dca.c
)

SET(LIBDCA_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdcadec.a
)

SET(LIBDCA_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libdca
        "${LIBDCA_SRC}"
        "${LIBDCA_LIBS}"
        []
        ""
        "${LIBDCA_INC}"
        ""
        "1")
