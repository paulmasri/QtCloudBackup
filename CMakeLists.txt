cmake_minimum_required(VERSION 3.21)

if(APPLE)
    project(QtCloudBackup LANGUAGES CXX OBJCXX)
else()
    project(QtCloudBackup LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Quick Concurrent)
qt_standard_project_setup(REQUIRES 6.8)

# Build-time configuration from consuming app
set(QTCLOUDBACKUP_APP_NAME "QtCloudBackup" CACHE STRING "App name for QtCloudBackup")
set(QTCLOUDBACKUP_ICLOUD_CONTAINER "" CACHE STRING "iCloud container identifier")
set(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH "" CACHE STRING "Relative path within OneDrive for backups")

set(SOURCES
    src/cloudbackupmanager.h
    src/cloudbackupmanager.cpp
    src/cloudbackupbackend.h
    src/backupinfo.h
    src/platform/fallback/localbackend.h
    src/platform/fallback/localbackend.cpp
)

if(APPLE)
    list(APPEND SOURCES
        src/platform/apple/appleicloudbackend.h
        src/platform/apple/appleicloudbackend.mm
    )
elseif(WIN32)
    list(APPEND SOURCES
        src/platform/windows/windowsonedrivebackend.h
        src/platform/windows/windowsonedrivebackend.cpp
    )
endif()

qt_add_library(QtCloudBackup STATIC ${SOURCES})

target_include_directories(QtCloudBackup PUBLIC src)

target_link_libraries(QtCloudBackup PUBLIC Qt6::Core Qt6::Quick PRIVATE Qt6::Concurrent)

target_compile_definitions(QtCloudBackup PRIVATE
    QTCLOUDBACKUP_APP_NAME="${QTCLOUDBACKUP_APP_NAME}"
    QTCLOUDBACKUP_ICLOUD_CONTAINER="${QTCLOUDBACKUP_ICLOUD_CONTAINER}"
    QTCLOUDBACKUP_WINDOWS_BACKUP_PATH="${QTCLOUDBACKUP_WINDOWS_BACKUP_PATH}"
)

if(APPLE)
    target_link_libraries(QtCloudBackup PRIVATE "-framework Foundation")
elseif(WIN32)
    target_link_libraries(QtCloudBackup PRIVATE shell32)
endif()

qt_add_qml_module(QtCloudBackup
    URI QtCloudBackup
    VERSION 1.0
    RESOURCE_PREFIX /
    SOURCES
        src/cloudbackupmanager.h
        src/cloudbackupmanager.cpp
        src/backupinfo.h
)
