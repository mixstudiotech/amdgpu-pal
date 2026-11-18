target_sources(palUtil PRIVATE
    winSync.cpp
    winThread.cpp
    winMemory.cpp
    winFileMap.cpp
    winLibrary.cpp
    winSysUtil.cpp
    winHashProvider.cpp
    winUuid.cpp
)
target_link_libraries(palUtil PRIVATE bcrypt user32 iphlpapi)

option(PAL_BUILD_WINDOWS_UTIL_TESTS "Build native Windows utility lifecycle tests" OFF)
if (PAL_BUILD_WINDOWS_UTIL_TESTS)
    enable_testing()
    add_executable(pal_windows_util_tests tests/winUtilTests.cpp)
    target_link_libraries(pal_windows_util_tests PRIVATE palUtil)
    target_compile_features(pal_windows_util_tests PRIVATE cxx_std_20)
    if (MSVC)
        target_compile_options(pal_windows_util_tests PRIVATE /Zc:__cplusplus)
    endif()
    add_test(NAME pal_windows_util_lifecycle COMMAND pal_windows_util_tests)
endif()
