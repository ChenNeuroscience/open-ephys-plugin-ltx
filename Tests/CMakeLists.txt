cmake_minimum_required(VERSION 3.16)
project(LTXRecordingTimingTests LANGUAGES CXX)
add_executable(recording-timing-test
    recording_timing_test.cpp
    ../Source/LTXRecordEnginePlugin.cpp
    ../Source/LTXFile.cpp)
target_compile_features(recording-timing-test PRIVATE cxx_std_17)
target_include_directories(recording-timing-test PRIVATE stubs ../Source)
if(MSVC)
    target_compile_definitions(recording-timing-test PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(recording-timing-test PRIVATE Threads::Threads)
endif()
