/*---------------------------------------------------------*\
| OpenRGBThermaltakeAIOPlugin.h                               |
|                                                             |
|   OpenRGB Thermaltake AIO round LCD Plugin                  |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include "LogManager.h"
#include "OpenRGBPluginInterface.h"
#include "ThermaltakeAIOTab.h"

class OpenRGBThermaltakeAIOPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "OpenRGBThermaltakeAIOPlugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    ~OpenRGBThermaltakeAIOPlugin() {};

    /*-----------------------------------------------------*\
    | Plugin Information                                    |
    \*-----------------------------------------------------*/
    virtual OpenRGBPluginInfo   GetPluginInfo()                                                                 override;
    virtual unsigned int        GetPluginAPIVersion()                                                           override;

    /*-----------------------------------------------------*\
    | Plugin Functionality                                  |
    \*-----------------------------------------------------*/
    virtual void                Load(OpenRGBPluginAPIInterface* api_interface_ptr)                              override;
    virtual QWidget*            GetWidget()                                                                     override;
    virtual QMenu*              GetTrayMenu()                                                                   override;
    virtual void                Unload()                                                                        override;
    virtual void                OnProfileAboutToLoad()                                                          override;
    virtual void                OnProfileLoad(nlohmann::json profile_data)                                      override;
    virtual nlohmann::json      OnProfileSave()                                                                 override;
    virtual unsigned char*      OnSDKCommand(unsigned int pkt_id, unsigned char* data, unsigned int* data_size) override;

    /*-----------------------------------------------------*\
    | Update Signals                                        |
    \*-----------------------------------------------------*/
    virtual void                ProfileManagerUpdated(unsigned int update_reason)                               override;
    virtual void                ResourceManagerUpdated(unsigned int update_reason)                              override;
    virtual void                SettingsManagerUpdated(unsigned int update_reason)                              override;

private:
    ThermaltakeAIOTab*          ui = nullptr;

public:
    static OpenRGBPluginAPIInterface* api;
};

/*---------------------------------------------------------*\
| LogManager logging macros -- plugins can't link against    |
| the host's LogManager directly, route through the API      |
| pointer instead (same pattern as OpenRGBEffectsPlugin)      |
\*---------------------------------------------------------*/
#undef  LogAppend
#define LogAppend(level, ...)   OpenRGBThermaltakeAIOPlugin::api->LogEntry(__FILE__, __LINE__, level, __VA_ARGS__)
