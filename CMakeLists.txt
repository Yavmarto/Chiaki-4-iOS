cmake_minimum_required(VERSION 3.25)
project(chiaki LANGUAGES C CXX OBJC OBJCXX VERSION 4.0)

# Build (see docs/BUILD-QT6.md):
#   device:    cmake -S . -B build-device -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#              -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/ios
#   simulator: same with -DCMAKE_OSX_SYSROOT=iphonesimulator
# Deployment target 16.0: Qt 6.8 LTS minimum on iOS.
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES arm64) # device + arm64 simulator, no x86_64
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Static XCFrameworks (device + arm64 simulator) from scripts/build-deps.sh
set(CHIAKI_FRAMEWORKS_DIR ${CMAKE_SOURCE_DIR}/Frameworks CACHE PATH "Dir with openssl/opus/sdl2/ffmpeg/chiaki .xcframework")
set(CHIAKI_DEVELOPMENT_TEAM "" CACHE STRING "Apple team id for signing")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Svg)
qt_standard_project_setup()

file(GLOB SOURCES CONFIGURE_DEPENDS *.cpp *.h)
list(FILTER SOURCES EXCLUDE REGEX "/moc_")

qt_add_executable(chiaki MACOSX_BUNDLE ${SOURCES} resources.qrc
    Media.xcassets LaunchScreen.storyboard)

target_include_directories(chiaki PRIVATE .)
target_compile_definitions(chiaki PRIVATE CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER=1 CHIAKI_VERSION="${PROJECT_VERSION}")
# CMake's Xcode generator picks the wrong XCFramework slice for the simulator, so use per-SDK paths.
foreach(dep chiaki openssl opus sdl2 ffmpeg)
    foreach(sdk iphoneos:ios-arm64 iphonesimulator:ios-arm64-simulator)
        string(REPLACE ":" ";" pair ${sdk})
        list(GET pair 0 sdkname)
        list(GET pair 1 slice)
        set(d ${CHIAKI_FRAMEWORKS_DIR}/${dep}.xcframework/${slice})
        string(APPEND LIB_PATHS_${sdkname} " \"${d}\"")
        string(APPEND INC_FLAGS_${sdkname} " -isystem \"${d}/Headers\"")
    endforeach()
endforeach()
foreach(sdkname iphoneos iphonesimulator)
    set_target_properties(chiaki PROPERTIES
        "XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS[sdk=${sdkname}*]" "$(inherited)${LIB_PATHS_${sdkname}}"
        "XCODE_ATTRIBUTE_OTHER_CPLUSPLUSFLAGS[sdk=${sdkname}*]" "$(inherited)${INC_FLAGS_${sdkname}}")
endforeach()
target_link_libraries(chiaki PRIVATE -lchiaki -lffmpeg -lopus -lopenssl -lsdl2)
target_link_libraries(chiaki PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets Qt6::Svg
    "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMedia"
    "-framework CoreVideo" "-framework VideoToolbox" "-framework GameController" "-framework CoreHaptics"
    "-framework CoreMotion" "-framework CoreBluetooth" "-framework CoreFoundation" "-framework Foundation" "-framework UIKit" "-framework QuartzCore" "-framework Metal" "-framework OpenGLES"
    z bz2 iconv)

set_target_properties(chiaki PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist
    MACOSX_BUNDLE_GUI_IDENTIFIER org.chiaki.chiaki-ios
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${CMAKE_SOURCE_DIR}/chiaki.entitlements
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CHIAKI_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO)
