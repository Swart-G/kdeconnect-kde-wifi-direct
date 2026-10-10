kdeconnect_add_plugin(kdeconnect_wifidirect SOURCES wifidirectplugin.cpp)
target_link_libraries(kdeconnect_wifidirect kdeconnectcore Qt::DBus KF6::I18n)
