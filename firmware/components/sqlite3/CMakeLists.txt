# Read-only SQLite for AFC snapshots; the application's VFS overlays committed
# WAL frames, so no FATFS locking or shared-memory implementation is required.
set(sqlite_source "${CMAKE_CURRENT_LIST_DIR}/../../../.deps/sqlite3")
if(NOT EXISTS "${sqlite_source}/sqlite3.c")
    include(FetchContent)
    FetchContent_Declare(tab5_sqlite
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
        URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Populate(tab5_sqlite)
    set(sqlite_source "${tab5_sqlite_SOURCE_DIR}")
endif()
idf_component_register(SRCS "sqlite_port.c" "sqlite_os.c"
    INCLUDE_DIRS "${sqlite_source}")
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-unused-function
    -Wno-unused-parameter -Wno-sign-compare -Wno-maybe-uninitialized)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SQLITE_OS_OTHER=1 SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_WAL SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_DESERIALIZE
    SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_TEMP_STORE=3
    SQLITE_DEFAULT_CACHE_SIZE=-512 SQLITE_MAX_MMAP_SIZE=0)
# OMIT_DESERIALIZE also avoids installing SQLite's unused memdb VFS during
# global initialization, before the application snapshot VFS is registered.
