#-----------------------------------------------------------------------------------------------#
# OpenRGB Thermaltake AIO LCD Plugin QMake Project                                               #
#-----------------------------------------------------------------------------------------------#

QT += core gui widgets

if(greaterThan(QT_MAJOR_VERSION, 5)) {
QT += core5compat
}

DEFINES += OPENRGB_THERMALTAKE_AIO_PLUGIN_LIBRARY
TEMPLATE = lib

CONFIG += plugin silent c++17

#-----------------------------------------------------------------------------------------------#
# Application Configuration                                                                     #
#-----------------------------------------------------------------------------------------------#
VERSION_NUM = 0.1.0
VERSION_STR = 0.1.0

PROJECT_DESC = "Custom image/animation display for the Thermaltake AIO round LCD screen"
PROJECT_NAME = "Thermaltake AIO LCD Plugin"
PROJECT_URL  = "https://github.com/annoyingprompt24/OpenRGBThermaltakeAIOPlugin"

GIT_COMMIT_ID = $$system(git log -n 1 --pretty=format:"%H" 2>/dev/null)
isEmpty(GIT_COMMIT_ID): GIT_COMMIT_ID = "unknown"

#-----------------------------------------------------------------------------------------------#
# Inject vars in defines                                                                        #
#-----------------------------------------------------------------------------------------------#
DEFINES +=                                                                                      \
    VERSION_STR=\\"\"\"$$VERSION_STR\\"\"\"                                                     \
    GIT_COMMIT_ID=\\"\"\"$$GIT_COMMIT_ID\\"\"\"                                                 \
    PROJECT_DESC=\\"\"\"$$PROJECT_DESC\\"\"\"                                                   \
    PROJECT_NAME=\\"\"\"$$PROJECT_NAME\\"\"\"                                                   \
    PROJECT_URL=\\"\"\"$$PROJECT_URL\\"\"\"                                                     \

#-----------------------------------------------------------------------------------------------#
# Update version in plugin metadata json                                                        #
#-----------------------------------------------------------------------------------------------#
JSON_FILE_IN  = $$PWD/OpenRGBThermaltakeAIOPlugin.json.in
JSON_FILE_OUT = $$PWD/OpenRGBThermaltakeAIOPlugin.json

prebuild_json.target   = prebuild_json_target
prebuild_json.depends  = FORCE
prebuild_json.commands = $$QMAKE_STREAM_EDITOR -e \"s|VERSION_NUM|$$VERSION_NUM|g\"               \
                                                -e \"s|VERSION_STR|$$VERSION_STR|g\"               \
                                                -e \"s|GIT_COMMIT_ID|$$GIT_COMMIT_ID|g\"           \
                                                -e \"s|PROJECT_DESC|$$PROJECT_DESC|g\"             \
                                                -e \"s|PROJECT_NAME|$$PROJECT_NAME|g\"             \
                                                -e \"s|PROJECT_URL|$$PROJECT_URL|g\"                \
                                                $$JSON_FILE_IN > $$JSON_FILE_OUT

QMAKE_EXTRA_TARGETS += prebuild_json
PRE_TARGETDEPS      += prebuild_json_target

#-----------------------------------------------------------------------------------------------#
# OpenRGB Plugin SDK                                                                             #
#-----------------------------------------------------------------------------------------------#
INCLUDEPATH +=                                                                                   \
    OpenRGB                                                                                      \
    OpenRGB/dependencies/json                                                                    \
    OpenRGB/qt                                                                                   \
    OpenRGB/RGBController                                                                        \

#-----------------------------------------------------------------------------------------------#
# Plugin sources                                                                                 #
#-----------------------------------------------------------------------------------------------#
HEADERS +=                                                                                       \
    OpenRGBThermaltakeAIOPlugin.h                                                                \
    ThermaltakeAIODevice.h                                                                       \
    ThermaltakeAIOTab.h                                                                          \

SOURCES +=                                                                                       \
    OpenRGBThermaltakeAIOPlugin.cpp                                                              \
    ThermaltakeAIODevice.cpp                                                                     \
    ThermaltakeAIOTab.cpp                                                                        \

FORMS +=                                                                                         \
    ThermaltakeAIOTab.ui                                                                         \

#-----------------------------------------------------------------------------------------------#
# Linux-specific Configuration                                                                   #
#-----------------------------------------------------------------------------------------------#
unix:!macx {
    QMAKE_CXXFLAGS += -std=c++17
    CONFIG += link_pkgconfig

    #---------------------------------------------------------------------------------------#
    # Determine which hidapi to use based on availability, preferring hidraw then libusb    #
    #---------------------------------------------------------------------------------------#
    packagesExist(hidapi-hidraw) {
        PKGCONFIG += hidapi-hidraw
    } else {
        packagesExist(hidapi-libusb) {
            PKGCONFIG += hidapi-libusb
        } else {
            PKGCONFIG += hidapi
        }
    }

    target.path = $$PREFIX/lib/openrgb/plugins/
    INSTALLS   += target
}

#-----------------------------------------------------------------------------------------------#
# Windows Configuration                                                                          #
#-----------------------------------------------------------------------------------------------#
win32:CONFIG += QTPLUGIN c++17
