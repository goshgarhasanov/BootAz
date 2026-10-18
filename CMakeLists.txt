cmake_minimum_required(VERSION 3.19)
project(BootAz VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BOOTAZ_GUI "Build the Qt desktop app" ON)
option(BOOTAZ_TESTS "Build the tests" ON)

find_package(Threads REQUIRED)

# ---- Core: everything that reads images and writes drives. Plain C++ (no Qt), so the CLI and
# the GUI share it and it cross-compiles for Windows with MinGW. ----
set(CORE_SOURCES
    core/Util.h core/Util.cpp
    core/I18n.h core/I18n.cpp core/I18n_az.cpp
    core/Hash.h core/Hash.cpp
    core/File.h core/File.cpp
    core/Iso.h core/Iso.cpp
    core/Image.h core/Image.cpp
    core/Disk.h core/Disk_common.cpp
    core/Partition.h core/Partition.cpp
    core/TreeWriter.h core/TreeWriter.cpp
    core/Fat32.h core/Fat32.cpp
    core/BootCode.h core/BootCode.cpp
    core/Unattend.h core/Unattend.cpp
    core/Jobs.h core/Jobs.cpp
)
if(WIN32)
    list(APPEND CORE_SOURCES core/Disk_win.cpp)
else()
    list(APPEND CORE_SOURCES core/Disk_linux.cpp)
endif()

# Binary resources compiled into the core.
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
function(bootaz_embed input name)
    set(FILE ${input})
    set(NAME ${name})
    set(SYM bootaz_embed_${name})
    configure_file(${CMAKE_SOURCE_DIR}/tools/embed.cpp.in ${GEN_DIR}/${name}.cpp @ONLY)
    set_property(SOURCE ${GEN_DIR}/${name}.cpp APPEND PROPERTY OBJECT_DEPENDS ${input})
    list(APPEND CORE_SOURCES ${GEN_DIR}/${name}.cpp)
    set(CORE_SOURCES ${CORE_SOURCES} PARENT_SCOPE)
endfunction()
bootaz_embed(${CMAKE_SOURCE_DIR}/third_party/uefi-ntfs/uefi-ntfs.img uefiNtfsImage)
bootaz_embed(${CMAKE_SOURCE_DIR}/resources/bootaz.ico autorunIcon)

add_library(bootaz_core STATIC ${CORE_SOURCES})
target_include_directories(bootaz_core PUBLIC core)
target_compile_definitions(bootaz_core PUBLIC BOOTAZ_VERSION="${PROJECT_VERSION}")
target_link_libraries(bootaz_core PUBLIC Threads::Threads)

# Optional decompressors for .img.gz / .xz / .zst / .bz2 images.
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(bootaz_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(bootaz_core PUBLIC BOOTAZ_HAVE_ZLIB)
endif()
find_package(LibLZMA QUIET)
if(LIBLZMA_FOUND)
    target_link_libraries(bootaz_core PUBLIC LibLZMA::LibLZMA)
    target_compile_definitions(bootaz_core PUBLIC BOOTAZ_HAVE_LZMA)
endif()
find_package(BZip2 QUIET)
if(BZIP2_FOUND)
    target_link_libraries(bootaz_core PUBLIC BZip2::BZip2)
    target_compile_definitions(bootaz_core PUBLIC BOOTAZ_HAVE_BZIP2)
endif()
find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd libzstd)
if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    target_include_directories(bootaz_core PUBLIC ${ZSTD_INCLUDE_DIR})
    target_link_libraries(bootaz_core PUBLIC ${ZSTD_LIBRARY})
    target_compile_definitions(bootaz_core PUBLIC BOOTAZ_HAVE_ZSTD)
endif()

if(WIN32)
    target_compile_definitions(bootaz_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(bootaz_core PUBLIC setupapi cfgmgr32)
endif()

# ---- CLI ----
set(CLI_SOURCES cli/main.cpp)
if(WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/resources/version.rc.in ${GEN_DIR}/version.rc @ONLY)
    list(APPEND CLI_SOURCES resources/bootaz.rc ${GEN_DIR}/version.rc)
endif()
add_executable(bootaz-cli ${CLI_SOURCES})
target_link_libraries(bootaz-cli PRIVATE bootaz_core)
if(MINGW)
    target_link_options(bootaz-cli PRIVATE -municode -static)
endif()

# ---- Desktop app (Qt 6) ----
if(BOOTAZ_GUI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)
    set(GUI_SOURCES
        gui/main.cpp gui/MainWindow.h gui/MainWindow.cpp gui/Engine.h gui/Engine.cpp
        gui/Dialogs.h gui/Dialogs.cpp gui/Downloads.h gui/Downloads.cpp
        gui/Widgets.h gui/Widgets.cpp gui/Theme.h gui/Theme.cpp gui/Brand.h gui/Brand.cpp resources/bootaz.qrc)
    if(WIN32)
        list(APPEND GUI_SOURCES resources/bootaz-gui.rc ${GEN_DIR}/version.rc)   # icon, "run as Administrator", version
    endif()
    add_executable(BootAz ${GUI_SOURCES})
    target_include_directories(BootAz PRIVATE gui)
    target_link_libraries(BootAz PRIVATE bootaz_core Qt6::Widgets Qt6::Network Qt6::Concurrent)
    if(WIN32)
        set_target_properties(BootAz PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()

    # Icon / logo generator: cmake --build build --target make_icons && build/make_icons resources
    add_executable(make_icons EXCLUDE_FROM_ALL tools/make_icons.cpp gui/Brand.cpp)
    target_include_directories(make_icons PRIVATE gui)
    target_link_libraries(make_icons PRIVATE Qt6::Widgets)

    if(BOOTAZ_TESTS)
        add_executable(bootaz-engine-test tests/test_engine.cpp gui/Engine.cpp gui/Engine.h)
        target_include_directories(bootaz-engine-test PRIVATE gui)
        target_link_libraries(bootaz-engine-test PRIVATE bootaz_core Qt6::Core)
    endif()
endif()

if(BOOTAZ_TESTS)
    enable_testing()
    add_executable(bootaz-tests tests/test_core.cpp)
    target_link_libraries(bootaz-tests PRIVATE bootaz_core)
    add_test(NAME core COMMAND bootaz-tests)
    # Image based tests read sample files from BOOTAZ_TEST_DATA (tests/make_test_images.sh).
    if(DEFINED ENV{BOOTAZ_TEST_DATA})
        set_tests_properties(core PROPERTIES ENVIRONMENT "BOOTAZ_TEST_DATA=$ENV{BOOTAZ_TEST_DATA}")
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(NAME translations COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_i18n.py)
    endif()
    if(BOOTAZ_GUI AND DEFINED ENV{BOOTAZ_TEST_DATA})
        add_test(NAME engine COMMAND bootaz-engine-test $ENV{BOOTAZ_TEST_DATA}/rr.iso)
    endif()
endif()
