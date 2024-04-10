add_definitions(-DTRANSLATION_DOMAIN=\"kio5_perldoc\")

add_library(kio_perldoc MODULE)
set_target_properties(kio_perldoc PROPERTIES
    OUTPUT_NAME "perldoc"
)

ecm_setup_version(${PROJECT_VERSION}
    VARIABLE_PREFIX KIO_PERLDOC
    VERSION_HEADER version.h
)

target_sources(kio_perldoc PRIVATE
    perldoc.cpp
)

target_link_libraries(kio_perldoc
    KF6::I18n
    KF6::KIOCore
)

install(TARGETS kio_perldoc DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/kio)
install(PROGRAMS pod2html.pl DESTINATION ${KDE_INSTALL_DATADIR}/kio_perldoc)
