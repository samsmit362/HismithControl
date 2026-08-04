cmake_minimum_required(VERSION 3.12)

project(HismithControl VERSION 8.0 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OpenCV_INCLUDE_DIRS "$ENV{OPENCV_BUILD_PATH_X64}/include" CACHE STRING "Paths to OpenCV include dirs")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")

# Instruct the linker to create the .pdb file for Release configuration
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

find_package(Qt6 REQUIRED COMPONENTS Widgets Xml Network Core Gui Charts)

file(GLOB BUTTPLUGCPP_SRC_FILES    
    "3rdParty/buttplugCpp/src/*.cpp"
    "3rdParty/buttplugCpp/include/*.h"
)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        FastLatencyWindow.h
        mainwindow.ui
        ThreadedCapture.h
        HighPrecisionTimerGuard.h
        HereSphereSync.cpp
        HereSphereSync.h
		3rdParty/OpenCVDeviceEnumerator/DeviceEnumerator.cpp
		3rdParty/OpenCVDeviceEnumerator/DeviceEnumerator.h
		${BUTTPLUGCPP_SRC_FILES}
)

if(6 GREATER_EQUAL 6)
	set(app_icon_resource_windows "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    qt_add_executable(HismithControl
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
		${app_icon_resource_windows}
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

# Find dependencies
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(ixwebsocket REQUIRED)

target_include_directories(HismithControl PUBLIC
	 ${OpenCV_INCLUDE_DIRS}
)

add_library(OpenCVLib STATIC IMPORTED)
set_target_properties(OpenCVLib PROPERTIES 
             IMPORTED_LOCATION_DEBUG "$ENV{OPENCV_BUILD_PATH_X64}/x64/vc16/lib/opencv_world$ENV{OPENCV_LIBS_VER}d.lib"
             IMPORTED_LOCATION_RELEASE "$ENV{OPENCV_BUILD_PATH_X64}/x64/vc16/lib/opencv_world$ENV{OPENCV_LIBS_VER}.lib")

target_link_libraries(HismithControl
					PRIVATE Qt6::Widgets Qt6::Xml Qt6::Network Qt::Charts Qt::Core Qt::Gui
					PUBLIC ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json Threads::Threads bcrypt OpenCVLib)

# Clearing the global CMake flags from the default /EHsc so that it does not conflict with /EHa
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" CACHE STRING "Compiler flags" FORCE)

target_compile_options(HismithControl PRIVATE 
    /W4
    /EHa
)

set_target_properties(HismithControl PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER my.example.com
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
	VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Soft\\Installed\\Qt\\6.8.3\\msvc2022_64\\bin;$ENV{OPENCV_BUILD_PATH_X64}\\x64\\vc16\\bin;%PATH%"
)

add_custom_command(
  TARGET HismithControl POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy 
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    $<TARGET_FILE_DIR:HismithControl>)

# Resources:
set(systray_resource_files
	"images/icon.ico"
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
