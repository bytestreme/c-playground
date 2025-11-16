find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)

find_library(LIBURING_LIBRARY NAMES uring liburing)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUring REQUIRED_VARS LIBURING_LIBRARY LIBURING_INCLUDE_DIR)

if (LIBURING_FOUND)
    add_library(LibUring::LibUring UNKNOWN IMPORTED)
    set_target_properties(LibUring::LibUring PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INCLUDE_DIR}"
            IMPORTED_LOCATION "${LIBURING_LIBRARY}"
    )
endif ()
