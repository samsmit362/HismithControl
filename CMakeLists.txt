cmake_minimum_required (VERSION 3.8)
include("3rdParty/buttplugCpp/cmake/HunterGate.cmake")

HunterGate(
    URL "https://github.com/cpp-pm/hunter/archive/v0.24.18.tar.gz"
    SHA1 "1292e4d661e1770d6d6ca08c12c07cf34a0bf718"
)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(HismithControl VERSION 0.1 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OpenCV_INCLUDE_DIRS "$ENV{OPENCV_BUILD_PATH_X64}/include" CACHE STRING "Paths to OpenCV include dirs")

find_package(Qt6 REQUIRED COMPONENTS Widgets Xml Network)

file(GLOB BUTTPLUGCPP_SRC_FILES    
    "3rdParty/buttplugCpp/src/*.cpp"
    "3rdParty/buttplugCpp/include/*.h"
)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
		MyClosedFigure.cpp
		MyClosedFigure.h
		DataTypes.h
		3rdParty/OpenCVDeviceEnumerator/DeviceEnumerator.cpp
		3rdParty/OpenCVDeviceEnumerator/DeviceEnumerator.h
		${BUTTPLUGCPP_SRC_FILES}
)

if(6 GREATER_EQUAL 6)
    qt_add_executable(HismithControl
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET HismithControl APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(HismithControl SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(HismithControl
            ${PROJECT_SOURCES}
        )
    endif()
endif()

hunter_add_package(nlohmann_json)
hunter_add_package(ZLIB)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

target_include_directories(HismithControl PUBLIC
	 ${OpenCV_INCLUDE_DIRS}
)

add_library(OpenCVLib STATIC IMPORTED)
set_target_properties(OpenCVLib PROPERTIES 
             IMPORTED_LOCATION_DEBUG "$ENV{OPENCV_BUILD_PATH_X64}/x64/vc16/lib/opencv_world$ENV{OPENCV_LIBS_VER}d.lib"
             IMPORTED_LOCATION_RELEASE "$ENV{OPENCV_BUILD_PATH_X64}/x64/vc16/lib/opencv_world$ENV{OPENCV_LIBS_VER}.lib")

target_link_libraries(HismithControl
					PRIVATE Qt6::Widgets Qt6::Xml Qt6::Network
					PUBLIC ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json Threads::Threads OpenCVLib)

set_target_properties(HismithControl PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER my.example.com
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
	VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\6.2.4\\msvc2019_64\\bin;$ENV{OPENCV_BUILD_PATH_X64}\\x64\\vc16\\bin;%PATH%"
)

add_custom_command(
  TARGET HismithControl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy 
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    $<TARGET_FILE_DIR:HismithControl>)

# Resources:
set(systray_resource_files
	"images/icon.png"
	"images/folder.png"
)

qt6_add_resources(HismithControl "systray"
    PREFIX
        "/"
    FILES
        ${systray_resource_files}
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HismithControl)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(HismithControl)
endif()
