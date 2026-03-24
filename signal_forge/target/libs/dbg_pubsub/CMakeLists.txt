cmake_minimum_required(VERSION 3.14)

project(dbg_pubsub
    VERSION     1.0.0
    DESCRIPTION "Lightweight UDP pub/sub debug variable streaming library"
    LANGUAGES   C
)

# ── Language standard ────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ── Source files ─────────────────────────────────────────────────────────────
add_library(dbg_pubsub STATIC
    dbg_socket.c
    dbg_util.c
    dbg_publisher.c
    dbg_subscriber.c
)

# ── Public headers ───────────────────────────────────────────────────────────
# dbg_pubsub.h  — primary public API (publisher + subscriber)
# dbg_protocol.h — wire-format types (included by dbg_pubsub.h)
# dbg_socket.h  — socket abstraction (needed by callers who use dbg_addr_t)
# dbg_util.h    — utilities: timestamps, CRC, logging, ring buffer
target_include_directories(dbg_pubsub
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include/dbg_pubsub>
)

# ── Platform-specific compile definitions and link libraries ─────────────────
if(WIN32)
    target_compile_definitions(dbg_pubsub PUBLIC DBG_PLATFORM_WINDOWS)
    # ws2_32: Winsock2.  iphlpapi: not needed currently but common companion.
    target_link_libraries(dbg_pubsub PUBLIC ws2_32)
elseif(APPLE)
    # mach_timebase_info / mach_absolute_time are in the system framework.
    # No extra link libraries needed.
elseif(UNIX)
    # clock_gettime is in librt on older glibc (< 2.17); a no-op on modern
    # Linux where it is folded into libc.  Link unconditionally for safety.
    target_link_libraries(dbg_pubsub PUBLIC ${CMAKE_DL_LIBS})
    find_library(RT_LIB rt)
    if(RT_LIB)
        target_link_libraries(dbg_pubsub PUBLIC ${RT_LIB})
    endif()
endif()

# ── Compiler warnings ────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(dbg_pubsub PRIVATE /W3 /WX)
else()
    target_compile_options(dbg_pubsub PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter        # volatile void* ptr intentionally unused in some paths
    )
endif()

# ── Optional: silence MSVC POSIX name deprecation warnings ───────────────────
if(MSVC)
    target_compile_definitions(dbg_pubsub PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
endif()

# ══════════════════════════════════════════════════════════════════════════════
# INSTALL RULES
# ══════════════════════════════════════════════════════════════════════════════
include(GNUInstallDirs)

install(TARGETS dbg_pubsub
    EXPORT  dbg_pubsub_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    dbg_pubsub.h
    dbg_protocol.h
    dbg_socket.h
    dbg_util.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dbg_pubsub
)

install(EXPORT dbg_pubsub_targets
    FILE      dbg_pubsubConfig.cmake
    NAMESPACE dbg_pubsub::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dbg_pubsub
)

# ══════════════════════════════════════════════════════════════════════════════
# INTEGRATION USAGE SUMMARY (printed at configure time)
# ══════════════════════════════════════════════════════════════════════════════
message(STATUS "-----------------------------------------------------------")
message(STATUS "  dbg_pubsub ${PROJECT_VERSION}")
message(STATUS "  Platform : ${CMAKE_SYSTEM_NAME}")
if(WIN32)
    message(STATUS "  Backend  : Winsock2 (DBG_PLATFORM_WINDOWS)")
elseif(APPLE)
    message(STATUS "  Backend  : POSIX / mach_absolute_time")
else()
    message(STATUS "  Backend  : POSIX / CLOCK_MONOTONIC")
endif()
message(STATUS "")
message(STATUS "  To link against this library in another CMake project:")
message(STATUS "    find_package(dbg_pubsub REQUIRED)")
message(STATUS "    target_link_libraries(your_target PRIVATE dbg_pubsub::dbg_pubsub)")
message(STATUS "  Or, if added as a subdirectory:")
message(STATUS "    add_subdirectory(dbg_pubsub)")
message(STATUS "    target_link_libraries(your_target PRIVATE dbg_pubsub)")
message(STATUS "-----------------------------------------------------------")
