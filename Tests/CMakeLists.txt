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

# Optional regression against the GUI's real metadata allocation/copy code.
# No GUI application or hardware is needed to exercise pre-acquisition reads.
if(LTX_GUI_SOURCE)
    file(TO_CMAKE_PATH "${LTX_GUI_SOURCE}" LTX_GUI_SOURCE)
    set(JUCE_CORE "${LTX_GUI_SOURCE}/JuceLibraryCode/modules/juce_core")
    if(APPLE)
        enable_language(OBJCXX)
        set(JUCE_CORE_SOURCE "${JUCE_CORE}/juce_core.mm")
    else()
        set(JUCE_CORE_SOURCE "${JUCE_CORE}/juce_core.cpp")
    endif()
    add_executable(native-metadata-test
        native_metadata_test.cpp
        "${LTX_GUI_SOURCE}/Source/Processors/Settings/Metadata.cpp"
        "${JUCE_CORE_SOURCE}"
        "${JUCE_CORE}/juce_core_CompilationTime.cpp")
    target_compile_features(native-metadata-test PRIVATE cxx_std_17)
    target_include_directories(native-metadata-test PRIVATE
        ../Source
        "${LTX_GUI_SOURCE}/JuceLibraryCode"
        "${LTX_GUI_SOURCE}/JuceLibraryCode/modules"
        "${LTX_GUI_SOURCE}/Source/Processors/Settings")
    target_compile_definitions(native-metadata-test PRIVATE
        NDEBUG=1 JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_USE_CURL=0)
    if(MSVC)
        target_compile_definitions(native-metadata-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    elseif(APPLE)
        target_link_libraries(native-metadata-test PRIVATE
            "-framework Cocoa" "-framework IOKit" "-framework Security" "-framework CoreServices")
    else()
        target_link_libraries(native-metadata-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS} rt)
    endif()
endif()
