cmake_minimum_required(VERSION 3.22)
project(VstTestBench VERSION 0.10.1)
set(CMAKE_CXX_STANDARD 17)

# JUCE の場所（プラットフォーム別・-DJUCE_DIR で上書き可）
if(NOT DEFINED JUCE_DIR)
    if(APPLE)
        set(JUCE_DIR "$ENV{HOME}/JUCE")
    else()
        set(JUCE_DIR "C:/JUCE")
    endif()
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

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
)

if(MSVC)
    target_compile_options(VstTestBench PRIVATE /utf-8)
endif()

# --- Windows オーディオバックエンド (ASIO/WASAPI/DirectSound) は Windows のみ ---
if(WIN32)
    # ASIO: enable only if the Steinberg ASIO SDK is present
    set(ASIO_SDK_DIR "C:/SDKs/ASIOSDK" CACHE PATH "Steinberg ASIO SDK folder")
    if(EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        message(STATUS "ASIO SDK found -> enabling JUCE_ASIO (${ASIO_SDK_DIR})")
        target_include_directories(VstTestBench PRIVATE "${ASIO_SDK_DIR}/common")
        target_compile_definitions(VstTestBench PRIVATE JUCE_ASIO=1)
    else()
        message(WARNING "ASIO SDK not found at ${ASIO_SDK_DIR} -> falling back to WASAPI/DirectSound")
    endif()
    target_compile_definitions(VstTestBench PRIVATE JUCE_WASAPI=1 JUCE_DIRECTSOUND=1)
endif()

target_compile_definitions(VstTestBench PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1       # macOS で AU をホスト（Windows では無視される）
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
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
