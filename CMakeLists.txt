cmake_minimum_required(VERSION 3.22)
project(VstTestBench VERSION 0.10.1)
set(CMAKE_CXX_STANDARD 17)

add_subdirectory("C:/JUCE" "${CMAKE_CURRENT_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# --- ARA hosting (Celemony's Audio Random Access): enabled only if the SDK is there ---------------------
# Recursive clone of https://github.com/Celemony/ARA_SDK (JUCE 8 documents releases/2.1.0).
set(ARA_SDK_DIR "C:/dev/ara_sdk" CACHE PATH "Celemony ARA SDK folder")
if(EXISTS "${ARA_SDK_DIR}/ARA_API/ARAInterface.h")
    message(STATUS "ARA SDK found -> enabling JUCE_PLUGINHOST_ARA (${ARA_SDK_DIR})")
    juce_set_ara_sdk_path("${ARA_SDK_DIR}")
    set(VSTTB_HAS_ARA ON)
else()
    message(STATUS "ARA SDK not found at ${ARA_SDK_DIR} -> no ARA hosting")
endif()

juce_add_gui_app(VstTestBench
    PRODUCT_NAME "VST TestBench"
    COMPANY_NAME "SeiNagi"
    ICON_BIG   "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
)

juce_generate_juce_header(VstTestBench)

target_sources(VstTestBench PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/ControlCommands.cpp
)

if(MSVC)
    target_compile_options(VstTestBench PRIVATE /utf-8)
endif()

# --- ASIO: enable only if the Steinberg ASIO SDK is present ---------------
set(ASIO_SDK_DIR "C:/SDKs/ASIOSDK" CACHE PATH "Steinberg ASIO SDK folder")
if(EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
    message(STATUS "ASIO SDK found -> enabling JUCE_ASIO (${ASIO_SDK_DIR})")
    target_include_directories(VstTestBench PRIVATE "${ASIO_SDK_DIR}/common")
    target_compile_definitions(VstTestBench PRIVATE JUCE_ASIO=1)
else()
    message(WARNING "ASIO SDK not found at ${ASIO_SDK_DIR} -> falling back to WASAPI/DirectSound")
endif()

if(VSTTB_HAS_ARA)
    target_compile_definitions(VstTestBench PRIVATE JUCE_PLUGINHOST_ARA=1)
endif()

target_compile_definitions(VstTestBench PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:VstTestBench,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:VstTestBench,JUCE_VERSION>"
)

target_link_libraries(VstTestBench PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
