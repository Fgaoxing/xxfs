cmake_minimum_required(VERSION 3.16)
project(xxfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LIB_SOURCES
    lib/xxfs.c
    lib/xxfs_os.c
)

add_library(xxfs STATIC ${LIB_SOURCES})
target_include_directories(xxfs PUBLIC lib)

find_library(READLINE_LIBRARY NAMES readline)
if(READLINE_LIBRARY)
    message(STATUS "Found readline: ${READLINE_LIBRARY}")
    add_definitions(-DHAVE_READLINE)
endif()

add_executable(mkfs.xxfs mkfs/mkfs.c)
target_link_libraries(mkfs.xxfs PRIVATE xxfs pthread)

add_executable(fsck.xxfs fsck/fsck.c)
target_link_libraries(fsck.xxfs PRIVATE xxfs pthread)

add_executable(xxfs-shell shell/shell.c)
target_link_libraries(xxfs-shell PRIVATE xxfs pthread)
if(READLINE_LIBRARY)
    target_link_libraries(xxfs-shell PRIVATE readline)
endif()

add_executable(xxfs-bench bench/bench.c)
target_link_libraries(xxfs-bench PRIVATE xxfs pthread)

add_executable(posix-bench bench/posix_bench.c)
target_link_libraries(posix-bench PRIVATE pthread)

add_executable(test-basic test/test_basic.c)
target_link_libraries(test-basic PRIVATE xxfs pthread)

enable_testing()
add_test(NAME basic COMMAND test-basic)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test-basic
)
