cmake_minimum_required(VERSION 3.21)

set(QTCLOUDBACKUP_LIB_VERSION 0.5.0)

project(QtCloudBackup VERSION ${QTCLOUDBACKUP_LIB_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Qml Concurrent)
qt_standard_project_setup(REQUIRES 6.8)

# Build-time configuration from consuming app
set(QTCLOUDBACKUP_APP_NAME "QtCloudBackup" CACHE STRING "App name for QtCloudBackup")
set(QTCLOUDBACKUP_ICLOUD_CONTAINER "" CACHE STRING "iCloud container identifier")
set(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH "" CACHE STRING "Relative path within OneDrive for backups")

# OneDrive's "Personal Vault" is a locked virtual folder; writes fail while
# it is locked. The configured subfolder must not collide with it. This is a
# compile-time value set by the consuming app's build, so catch the mistake
# here rather than at runtime — by the time the backend notices, the binary
# has already shipped. Generally the subfolder should be a unique
# app-namespaced name.
if(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH STREQUAL "Personal Vault")
    message(FATAL_ERROR
        "QTCLOUDBACKUP_WINDOWS_BACKUP_PATH must not be 'Personal Vault' — "
        "this collides with OneDrive's locked virtual folder and writes "
        "will fail when the vault is locked. Choose a unique app-namespaced "
        "subfolder name.")
endif()

set(SOURCES
    src/cloudbackupmanager.h
    src/cloudbackupmanager.cpp
    src/cloudbackupbackend.h
    src/backupinfo.h
    src/backupvalidation.h
    src/retentionpolicy.h
    src/retentionevaluator.h
    src/retentionevaluator.cpp
    src/platform/fallback/localbackend.h
    src/platform/fallback/localbackend.cpp
)

if(APPLE)
    list(APPEND SOURCES
        src/platform/apple/appleicloudbackend.h
        src/platform/apple/appleicloudbackend.mm
    )
    set_source_files_properties(
        src/platform/apple/appleicloudbackend.mm
        PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
elseif(WIN32)
    list(APPEND SOURCES
        src/platform/windows/windowsonedrivebackend.h
        src/platform/windows/windowsonedrivebackend.cpp
    )
endif()

qt_add_library(QtCloudBackup STATIC ${SOURCES})

target_include_directories(QtCloudBackup PUBLIC src)

target_link_libraries(QtCloudBackup PUBLIC Qt6::Core Qt6::Qml PRIVATE Qt6::Concurrent)

target_compile_definitions(QtCloudBackup PRIVATE
    QTCLOUDBACKUP_APP_NAME="${QTCLOUDBACKUP_APP_NAME}"
    QTCLOUDBACKUP_ICLOUD_CONTAINER="${QTCLOUDBACKUP_ICLOUD_CONTAINER}"
    QTCLOUDBACKUP_WINDOWS_BACKUP_PATH="${QTCLOUDBACKUP_WINDOWS_BACKUP_PATH}"
    QTCLOUDBACKUP_VERSION="${PROJECT_VERSION}"
)

if(APPLE)
    target_link_libraries(QtCloudBackup PRIVATE "-framework Foundation")
endif()

option(QTCLOUDBACKUP_BUILD_TESTS "Build QtCloudBackup ad-hoc test executables" OFF)
if(QTCLOUDBACKUP_BUILD_TESTS)
    qt_add_executable(test_retention tools/test_retention.cpp)
    target_link_libraries(test_retention PRIVATE QtCloudBackup Qt6::Core)
endif()

qt_add_qml_module(QtCloudBackup
    URI QtCloudBackup
    VERSION 1.0
    RESOURCE_PREFIX /
    SOURCES
        src/cloudbackupmanager.h
        src/cloudbackupmanager.cpp
        src/backupinfo.h
        src/retentionpolicy.h
)
