# Sh4ltVendor.cmake — builds sh4lt as an in-tree static library.
# Expects the submodule at ${CMAKE_SOURCE_DIR}/3rdparty/sh4lt.
#
# After include(), the imported target sh4lt_vendor is available.
# Callers must link against it and will automatically get the include path.

set(_SH4LT_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/sh4lt")

if(NOT EXISTS "${_SH4LT_ROOT}/sh4lt/writer.hpp")
    message(WARNING
        "sh4lt submodule not found at ${_SH4LT_ROOT}. "
        "Run: git submodule update --init --recursive. "
        "Sh4lt output will be disabled.")
    return()
endif()

set(_SH4LT_SOURCES
    ${_SH4LT_ROOT}/sh4lt/follower.cpp
    ${_SH4LT_ROOT}/sh4lt/infotree/information-tree.cpp
    ${_SH4LT_ROOT}/sh4lt/infotree/json-serializer.cpp
    ${_SH4LT_ROOT}/sh4lt/infotree/key-val-serializer.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/file-monitor.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/reader.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/sysv-sem.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/sysv-shm.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/unix-socket-client.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/unix-socket-protocol.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/unix-socket-server.cpp
    ${_SH4LT_ROOT}/sh4lt/ipcs/unix-socket.cpp
    ${_SH4LT_ROOT}/sh4lt/jsoncpp/jsoncpp.cpp
    ${_SH4LT_ROOT}/sh4lt/monitor/dir-watcher.cpp
    ${_SH4LT_ROOT}/sh4lt/monitor/follower-stat.cpp
    ${_SH4LT_ROOT}/sh4lt/monitor/monitor.cpp
    ${_SH4LT_ROOT}/sh4lt/shtype/shtype-from-gst-caps.cpp
    ${_SH4LT_ROOT}/sh4lt/shtype/shtype.cpp
    ${_SH4LT_ROOT}/sh4lt/time.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/any.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/bool-log.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/data-printer.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/safe-bool-log.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/serialize-string.cpp
    ${_SH4LT_ROOT}/sh4lt/utils/string-utils.cpp
    ${_SH4LT_ROOT}/sh4lt/writer.cpp
    # C wrappers (needed transitively by some internals)
    ${_SH4LT_ROOT}/sh4lt/c/cfollower.cpp
    ${_SH4LT_ROOT}/sh4lt/c/clogger.cpp
    ${_SH4LT_ROOT}/sh4lt/c/cshtype.cpp
    ${_SH4LT_ROOT}/sh4lt/c/cwriter.cpp
)

add_library(sh4lt_vendor STATIC ${_SH4LT_SOURCES})

set_target_properties(sh4lt_vendor PROPERTIES
    CXX_STANDARD 20
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(sh4lt_vendor PRIVATE
    -DSH4LT_VERSION_STRING="0.2.0"
    # Suppress warnings from third-party code
    -w
)

target_include_directories(sh4lt_vendor PUBLIC
    ${_SH4LT_ROOT}   # so #include "sh4lt/writer.hpp" resolves
)

target_link_libraries(sh4lt_vendor PUBLIC pthread rt)

set(HAVE_SH4LT_VENDOR TRUE)
