cmake_minimum_required(VERSION 3.20)

project(hpmalloc VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The allocator is deliberately split into translation units along its
# existing internal boundaries: global state, arena/page management, central
# bins, large/direct mappings, runtime hooks/options, the malloc-family slow
# paths, and the exported compatibility API.
set(HPMALLOC_CORE_SOURCES
    src/globals.cpp
    src/arena.cpp
    src/central.cpp
    src/large.cpp
    src/runtime.cpp
    src/malloc.cpp
    src/api.cpp
)
set(HPMALLOC_SOURCES
    ${HPMALLOC_CORE_SOURCES}
    src/cxx_new.cpp
)

set(HPMALLOC_PUBLIC_INCLUDE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

function(hpmalloc_common_options target)
    target_include_directories(${target}
        PUBLIC ${HPMALLOC_PUBLIC_INCLUDE}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE
        -O3
        -flto
        -march=native
        -mtune=native
        -DNDEBUG
        -Wall
        -Wextra
        -fno-rtti
        -fvisibility=hidden
        -fvisibility-inlines-hidden)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE -fno-semantic-interposition)
    endif()
endfunction()

add_library(hpmalloc SHARED ${HPMALLOC_SOURCES})
hpmalloc_common_options(hpmalloc)
set_source_files_properties(${HPMALLOC_CORE_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
set_source_files_properties(src/cxx_new.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
set_target_properties(hpmalloc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    OUTPUT_NAME hpmalloc)
target_link_options(hpmalloc PRIVATE
    -Wl,-O3
    -Wl,-z,now
    -Wl,-z,relro
    -Wl,-z,nodelete)

add_library(hpmalloc_static STATIC ${HPMALLOC_SOURCES})
hpmalloc_common_options(hpmalloc_static)
set_source_files_properties(${HPMALLOC_CORE_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
set_source_files_properties(src/cxx_new.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
set_target_properties(hpmalloc_static PROPERTIES OUTPUT_NAME hpmalloc)

include(GNUInstallDirs)
install(TARGETS hpmalloc hpmalloc_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/hpmalloc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

option(HPM_BUILD_BENCH "Build the allocator-agnostic benchmark" ON)
if(HPM_BUILD_BENCH)
    find_package(Threads REQUIRED)
    add_executable(hpmalloc_bench bench/alloc_bench.cpp)
    target_link_libraries(hpmalloc_bench PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(hpmalloc_bench PRIVATE
        -O3 -DNDEBUG -Wall -Wextra
        -march=native -mtune=native
        -fno-builtin-malloc -fno-builtin-calloc
        -fno-builtin-realloc -fno-builtin-free)
endif()

option(HPM_BUILD_TESTS "Build hpmalloc tests" ON)
if(HPM_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(hpmalloc_basic_test tests/test_basic.cpp)
    target_link_libraries(hpmalloc_basic_test PRIVATE hpmalloc)
    target_include_directories(hpmalloc_basic_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(hpmalloc_basic_test PRIVATE -O2 -UNDEBUG)
    add_test(NAME hpmalloc_basic COMMAND hpmalloc_basic_test)

    add_executable(hpmalloc_preload_cpp_test tests/test_preload_cpp.cpp)
    target_link_libraries(hpmalloc_preload_cpp_test PRIVATE Threads::Threads)
    target_compile_options(hpmalloc_preload_cpp_test PRIVATE -O2 -UNDEBUG)
    add_test(NAME hpmalloc_preload_cpp
        COMMAND ${CMAKE_COMMAND} -E env
            "LD_PRELOAD=$<TARGET_FILE:hpmalloc>"
            $<TARGET_FILE:hpmalloc_preload_cpp_test>)

    add_test(NAME hpmalloc_compat_coreutils
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/compat_coreutils.sh
                $<TARGET_FILE:hpmalloc>)

    add_test(NAME hpmalloc_compat_common
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/compat_common.sh
                $<TARGET_FILE:hpmalloc>)

    set_tests_properties(hpmalloc_preload_cpp PROPERTIES TIMEOUT 120)
    set_tests_properties(hpmalloc_compat_coreutils PROPERTIES TIMEOUT 240)
    set_tests_properties(hpmalloc_compat_common PROPERTIES TIMEOUT 300)
endif()
