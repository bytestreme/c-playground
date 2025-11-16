include(FindPackageHandleStandardArgs)

find_path(LIBEVENT_INCLUDE_DIR
        NAMES event2/event.h
        PATH_SUFFIXES include
)

find_library(LIBEVENT_LIB NAMES event)
find_library(LIBEVENT_CORE_LIB NAMES event_core)
find_library(LIBEVENT_EXTRA_LIB NAMES event_extra)
find_library(LIBEVENT_PTHREADS_LIB NAMES event_pthreads)

set(_system_libevent_found FALSE)

if (LIBEVENT_INCLUDE_DIR AND LIBEVENT_LIB AND LIBEVENT_CORE_LIB
        AND LIBEVENT_EXTRA_LIB AND LIBEVENT_PTHREADS_LIB)

    set(_system_libevent_found TRUE)
endif ()

if (NOT _system_libevent_found)
    message(STATUS "System libevent not found. Fetching libevent via FetchContent...")

    include(FetchContent)

    FetchContent_Declare(
            libevent
            GIT_REPOSITORY https://github.com/libevent/libevent.git
            GIT_TAG release-2.1.12-stable
    )

    set(EVENT__DISABLE_TESTS ON CACHE INTERNAL "")
    set(EVENT__DISABLE_REGRESS ON CACHE INTERNAL "")
    set(EVENT__DISABLE_SAMPLES ON CACHE INTERNAL "")
    set(EVENT__DISABLE_BENCHMARK ON CACHE INTERNAL "")

    FetchContent_MakeAvailable(libevent)

    set(LIBEVENT_INCLUDE_DIR ${libevent_SOURCE_DIR}/include)
    set(LIBEVENT_LIB event)
    set(LIBEVENT_CORE_LIB event_core)
    set(LIBEVENT_EXTRA_LIB event_extra)
    set(LIBEVENT_PTHREADS_LIB event_pthreads)
endif ()

find_package_handle_standard_args(
        Libevent
        REQUIRED_VARS
        LIBEVENT_INCLUDE_DIR
        LIBEVENT_LIB
        LIBEVENT_CORE_LIB
        LIBEVENT_EXTRA_LIB
        LIBEVENT_PTHREADS_LIB
)

if (NOT LIBEVENT_FOUND)
    return()
endif ()

set(LIBEVENT_INCLUDE_DIRS ${LIBEVENT_INCLUDE_DIR})
set(LIBEVENT_LIBRARIES
        ${LIBEVENT_LIB}
        ${LIBEVENT_CORE_LIB}
        ${LIBEVENT_EXTRA_LIB}
        ${LIBEVENT_PTHREADS_LIB}
)

if (NOT TARGET Libevent::Libevent)
    add_library(Libevent::Libevent INTERFACE IMPORTED)

    set_target_properties(Libevent::Libevent PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${LIBEVENT_LIBRARIES}"
    )
endif ()

