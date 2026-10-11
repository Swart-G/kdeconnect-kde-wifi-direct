find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

set(kdeconnect_libraries
    kdeconnectcore
    kdeconnectdbusinterfaces
    kdeconnectmodels
    kdeconnectsmshelper
    kdeconnectversion
    KF6::GuiAddons
    KF6::I18n
    KF6::KIOWidgets
    KF6::Notifications
    Qt::DBus
    Qt::Network
    KF6::People
    Qt::Qml
    Qt::Test
)

ecm_add_test(notificationstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(pluginloadtest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(sendfiletest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
target_sources(sendfiletest PRIVATE testdevice.cpp)
ecm_add_test(smshelpertest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
ecm_add_test(wifidirecttest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
if (WIN32 OR APPLE)
    ecm_add_test(mdnstest.cpp LINK_LIBRARIES ${kdeconnect_libraries})
endif()
ecm_add_test(dbusresponsetest.cpp ${PROJECT_SOURCE_DIR}/declarativeplugin/responsewaiter.cpp TEST_NAME dbusresponsetest LINK_LIBRARIES ${kdeconnect_libraries})
add_executable(wifidirectdiagnostic wifidirectdiagnostic.cpp)
target_link_libraries(wifidirectdiagnostic PRIVATE kdeconnectcore Qt::Core)
ecm_add_test(lantransporttest.cpp TEST_NAME lantransporttest LINK_LIBRARIES ${kdeconnect_libraries})
target_sources(lantransporttest PRIVATE ${PROJECT_SOURCE_DIR}/core/sslhelper.cpp ${PROJECT_BINARY_DIR}/core/kdeconnect_debug.cpp)
