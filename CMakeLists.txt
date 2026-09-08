cmake_minimum_required(VERSION 3.24)

project(ChurchStreamProcessor VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

option(CSP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    # Keep the release executable self-contained; the installer does not need
    # to add a separate Visual C++ runtime package.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Compatibility floor for pinned C dependencies" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "Disable dependency tests" FORCE)
set(ENABLE_INTERNAL_QUEUE_H ON CACHE BOOL "Use bundled queue implementation" FORCE)
FetchContent_Declare(
    libebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    GIT_TAG v1.2.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libebur128)

juce_add_gui_app(ChurchStreamProcessor
    PRODUCT_NAME "Church Stream Processor"
    COMPANY_NAME "Church Stream Processor"
    BUNDLE_ID "org.churchstream.processor"
    VERSION ${PROJECT_VERSION}
    ICON_BIG assets/church-stream-icon.png
    ICON_SMALL assets/church-stream-icon.png
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Church Stream Processor needs microphone/audio-input access to receive the X32 stereo mix."
)

target_sources(ChurchStreamProcessor PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/MainComponent.h
    src/OBS/LocalWebSocket.cpp
    src/OBS/LocalWebSocket.h
    src/OBS/OBSAuthentication.h
    src/OBS/OBSController.cpp
    src/OBS/OBSController.h
    src/Offline/OfflineProcessor.cpp
    src/Offline/OfflineProcessor.h
    src/Audio/AudioEngine.cpp
    src/Audio/AudioEngine.h
    src/Audio/AudioRouting.h
    src/Audio/MeterSource.h
    src/Analysis/AnalysisEngine.cpp
    src/Analysis/AnalysisEngine.h
    src/Analysis/AnalysisTypes.h
    src/Analysis/StereoAudioFifo.h
    src/DSP/Biquad.h
    src/DSP/DspParameters.h
    src/DSP/ProcessingEngine.cpp
    src/DSP/ProcessingEngine.h
    src/DSP/TruePeakDetector.h
    src/Settings/AppSettings.cpp
    src/Settings/AppSettings.h
    src/Settings/PresetManager.cpp
    src/Settings/PresetManager.h
    src/Smart/SmartEngine.cpp
    src/Smart/SmartEngine.h
    src/Smart/SmartTypes.h
    src/Platform/OBSLocator.cpp
    src/Platform/OBSLocator.h
    src/Platform/StartupManager.cpp
    src/Platform/StartupManager.h
    src/Platform/WindowsAudioEndpoint.cpp
    src/Platform/WindowsAudioEndpoint.h
    src/System/SystemMonitor.cpp
    src/System/SystemMonitor.h
    src/Safety/SafetyController.cpp
    src/Safety/SafetyController.h
    src/Diagnostics/SessionDiagnostics.cpp
    src/Diagnostics/SessionDiagnostics.h
    src/Groups/SmartMaskingController.cpp
    src/Groups/SmartMaskingController.h
    src/Groups/AutoGroupRouter.cpp
    src/Groups/AutoGroupRouter.h
    src/Groups/GroupMixer.cpp
    src/Groups/GroupMixer.h
    src/X32/OscMessage.h
    src/X32/X32Client.cpp
    src/X32/X32Client.h
    src/Room/RoomCalibration.cpp
    src/Room/RoomCalibration.h
    src/UI/LevelMeter.cpp
    src/UI/LevelMeter.h
    src/UI/SpectrumComponent.cpp
    src/UI/SpectrumComponent.h
    src/UI/TrayController.cpp
    src/UI/TrayController.h
    src/UI/Theme.cpp
    src/UI/Theme.h
)

target_include_directories(ChurchStreamProcessor PRIVATE
    src
    ${libebur128_SOURCE_DIR}/ebur128
)
target_compile_features(ChurchStreamProcessor PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(ChurchStreamProcessor PRIVATE /utf-8 /permissive-)
elseif(CSP_ENABLE_SANITIZERS)
    target_compile_options(ChurchStreamProcessor PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ChurchStreamProcessor PRIVATE -fsanitize=address,undefined)
endif()

target_compile_definitions(ChurchStreamProcessor PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ChurchStreamProcessor,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=0
)

target_link_libraries(ChurchStreamProcessor PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_cryptography
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    ebur128
)

# The workspace may live in a cloud-synchronised Desktop folder that adds
# Finder metadata during builds. CI/release signing is handled after staging.
if(APPLE)
    set_target_properties(ChurchStreamProcessor PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    )
endif()

if(WIN32)
    target_link_libraries(ChurchStreamProcessor PRIVATE Psapi Ole32 Uuid Crypt32)
endif()

if(BUILD_TESTING)
    juce_add_console_app(ChurchStreamProcessorTests
        PRODUCT_NAME "Church Stream Processor Tests"
    )

    target_sources(ChurchStreamProcessorTests PRIVATE
        tests/AudioCoreTests.cpp
        src/Analysis/AnalysisEngine.cpp
        src/Analysis/AnalysisEngine.h
        src/Analysis/AnalysisTypes.h
        src/Analysis/StereoAudioFifo.h
        src/DSP/Biquad.h
        src/DSP/DspParameters.h
        src/DSP/ProcessingEngine.cpp
        src/DSP/ProcessingEngine.h
        src/DSP/TruePeakDetector.h
        src/Smart/SmartEngine.cpp
        src/Smart/SmartEngine.h
        src/Smart/SmartTypes.h
        src/Offline/OfflineProcessor.cpp
        src/Offline/OfflineProcessor.h
        src/OBS/OBSAuthentication.h
        src/Audio/AudioRouting.h
        src/Audio/MeterSource.h
        src/Safety/SafetyController.cpp
        src/Safety/SafetyController.h
        src/Groups/SmartMaskingController.cpp
        src/Groups/SmartMaskingController.h
        src/Groups/AutoGroupRouter.cpp
        src/Groups/AutoGroupRouter.h
        src/Groups/GroupMixer.cpp
        src/Groups/GroupMixer.h
        src/X32/OscMessage.h
        src/X32/X32Client.cpp
        src/X32/X32Client.h
        src/Room/RoomCalibration.cpp
        src/Room/RoomCalibration.h
    )

    target_include_directories(ChurchStreamProcessorTests PRIVATE
        src
        ${libebur128_SOURCE_DIR}/ebur128
    )
    target_compile_features(ChurchStreamProcessorTests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ChurchStreamProcessorTests PRIVATE /utf-8 /permissive-)
    elseif(CSP_ENABLE_SANITIZERS)
        target_compile_options(ChurchStreamProcessorTests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(ChurchStreamProcessorTests PRIVATE -fsanitize=address,undefined)
    endif()
    target_compile_definitions(ChurchStreamProcessorTests PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )
    target_link_libraries(ChurchStreamProcessorTests PRIVATE
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_cryptography
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
        ebur128
    )

    add_test(NAME AudioCoreTests COMMAND ChurchStreamProcessorTests)

    juce_add_console_app(ChurchStreamProcessorBenchmark
        PRODUCT_NAME "Church Stream Processor Benchmark"
    )
    target_sources(ChurchStreamProcessorBenchmark PRIVATE
        tests/ProcessingBenchmark.cpp
        src/DSP/Biquad.h
        src/DSP/DspParameters.h
        src/DSP/ProcessingEngine.cpp
        src/DSP/ProcessingEngine.h
        src/DSP/TruePeakDetector.h
    )
    target_include_directories(ChurchStreamProcessorBenchmark PRIVATE src)
    target_compile_features(ChurchStreamProcessorBenchmark PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(ChurchStreamProcessorBenchmark PRIVATE /utf-8 /permissive-)
    elseif(CSP_ENABLE_SANITIZERS)
        target_compile_options(ChurchStreamProcessorBenchmark PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(ChurchStreamProcessorBenchmark PRIVATE -fsanitize=address,undefined)
    endif()
    target_compile_definitions(ChurchStreamProcessorBenchmark PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(ChurchStreamProcessorBenchmark PRIVATE
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    )

    add_test(
        NAME VirtualDriverSourceManifest
        COMMAND ${CMAKE_COMMAND}
            -DCSP_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ValidateDriverSource.cmake
    )
endif()
