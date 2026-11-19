cmake_minimum_required(VERSION 3.21)
project(WirelessBatteryPanel LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Qt Quick panel is available on Windows.")
endif()
enable_language(RC)

find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2 Network LinguistTools)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(WirelessBatteryPanel WIN32
    panel/main.cpp
    panel/devicemodel.cpp
    panel/devicemodel.h
    panel/windowframe.cpp
    panel/windowframe.h
    panel/panel.rc
)
target_compile_features(WirelessBatteryPanel PRIVATE cxx_std_17)
target_include_directories(WirelessBatteryPanel PRIVATE panel res)
target_compile_definitions(WirelessBatteryPanel PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
target_link_libraries(WirelessBatteryPanel PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network comctl32 dwmapi)
qt_add_resources(WirelessBatteryPanel branding
    PREFIX "/branding"
    BASE res
    FILES res/app.svg res/app.ico
)

file(GLOB LUCIDE_ICONS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "panel/assets/icons/lucide/*.svg")
configure_file(panel/assets/icons/lucide/LICENSE LUCIDE-LICENSE.txt COPYONLY)
qt_add_qml_module(WirelessBatteryPanel
    URI WirelessBattery
    QML_FILES panel/Main.qml
    RESOURCES ${LUCIDE_ICONS}
)
qt_add_translations(WirelessBatteryPanel
    TS_FILES panel/i18n/panel_zh_CN.ts panel/i18n/panel_zh_TW.ts
    RESOURCE_PREFIX "/i18n"
)
