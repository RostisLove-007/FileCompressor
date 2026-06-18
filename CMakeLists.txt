cmake_minimum_required(VERSION 3.16)
project(FileCompressor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ──────────────────────────────────────────────────────────────
#  Опции
# ──────────────────────────────────────────────────────────────
option(BUILD_TESTS "Собирать тесты (Google Test)" ON)
option(BUILD_GUI   "Собирать GUI-приложение (требует GTK3)" ON)

# ──────────────────────────────────────────────────────────────
#  Заголовочная библиотека с алгоритмами (header-only, без GTK)
# ──────────────────────────────────────────────────────────────
add_library(filecompressor_core INTERFACE)
target_include_directories(filecompressor_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(filecompressor_core INTERFACE cxx_std_17)

# ──────────────────────────────────────────────────────────────
#  GUI-приложение (GTK3)
# ──────────────────────────────────────────────────────────────
if(BUILD_GUI)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK3 gtk+-3.0)
    endif()

    if(GTK3_FOUND)
        add_executable(filecompressor src/main.cpp)
        target_link_libraries(filecompressor PRIVATE filecompressor_core)

        target_include_directories(filecompressor PRIVATE ${GTK3_INCLUDE_DIRS})
        target_compile_options(filecompressor PRIVATE ${GTK3_CFLAGS_OTHER})
        target_link_directories(filecompressor PRIVATE ${GTK3_LIBRARY_DIRS})
        target_link_libraries(filecompressor PRIVATE ${GTK3_LIBRARIES})

        find_package(Threads REQUIRED)
        target_link_libraries(filecompressor PRIVATE Threads::Threads)

        message(STATUS "GUI-приложение filecompressor будет собрано (найден GTK3).")
    else()
        message(WARNING "GTK3 не найден (pkg-config: gtk+-3.0). "
                         "Цель 'filecompressor' пропущена. "
                         "На macOS: brew install gtk+3 pkg-config. "
                         "На Linux: sudo apt install libgtk-3-dev pkg-config.")
    endif()
endif()

# ──────────────────────────────────────────────────────────────
#  Тесты (Google Test)
# ──────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()

    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        )
        # На macOS форсируем динамическую runtime-библиотеку,
        # чтобы совпадать с настройками проекта по умолчанию.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(filecompressor_tests tests/tests.cpp)
    target_include_directories(filecompressor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(GTest_FOUND)
        target_link_libraries(filecompressor_tests PRIVATE GTest::gtest GTest::gtest_main)
    else()
        target_link_libraries(filecompressor_tests PRIVATE gtest gtest_main)
    endif()

    find_package(Threads REQUIRED)
    target_link_libraries(filecompressor_tests PRIVATE Threads::Threads)

    include(GoogleTest)
    gtest_discover_tests(filecompressor_tests)
endif()
