cmake_minimum_required(VERSION 3.16)

# Release service version, managed by release script
set (RELEASE_SERVICE_VERSION_MAJOR "23")
set (RELEASE_SERVICE_VERSION_MINOR "07")
set (RELEASE_SERVICE_VERSION_MICRO "70")
set (RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(kio-kdesdk VERSION ${RELEASE_SERVICE_VERSION})

set(QT_MIN_VERSION 5.15.2)
set(KF5_MIN_VERSION 5.96.0)

find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

include(ECMSetupVersion)
include(ECMDeprecationSettings)

include(FeatureSummary)

find_package(Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} REQUIRED NO_MODULE
    COMPONENTS
    Core
)

find_package(KF5 ${KF5_MIN_VERSION} REQUIRED
    COMPONENTS
    I18n
    KIO
)

find_package(Perl 5.10)

ecm_set_disabled_deprecation_versions(
    QT 5.15.2
    KF 5.96
)

if(PERL_FOUND)
    add_subdirectory(perldoc)
endif(PERL_FOUND)

ki18n_install(po)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
