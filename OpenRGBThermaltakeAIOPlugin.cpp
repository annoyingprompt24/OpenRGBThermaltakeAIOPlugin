/*---------------------------------------------------------*\
| OpenRGBThermaltakeAIOPlugin.cpp                             |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "OpenRGBThermaltakeAIOPlugin.h"

OpenRGBPluginAPIInterface* OpenRGBThermaltakeAIOPlugin::api = nullptr;

/*-----------------------------------------------------*\
| Plugin Information                                    |
\*-----------------------------------------------------*/
OpenRGBPluginInfo OpenRGBThermaltakeAIOPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name            = PROJECT_NAME;
    info.Description     = PROJECT_DESC;
    info.Version          = VERSION_STR;
    info.Commit           = GIT_COMMIT_ID;
    info.URL              = PROJECT_URL;

    info.Label            = "Thermaltake AIO LCD";
    info.Location         = OPENRGB_PLUGIN_LOCATION_TOP;

    info.ProtocolVersion  = 1;

    return(info);
}

unsigned int OpenRGBThermaltakeAIOPlugin::GetPluginAPIVersion()
{
    return(OPENRGB_PLUGIN_API_VERSION);
}

/*-----------------------------------------------------*\
| Plugin Functionality                                  |
\*-----------------------------------------------------*/
void OpenRGBThermaltakeAIOPlugin::Load(OpenRGBPluginAPIInterface* api_interface_ptr)
{
    api = api_interface_ptr;

    LOG_INFO("[ThermaltakeAIOPlugin] loaded, version %s (%s)", VERSION_STR, GIT_COMMIT_ID);

    ui = new ThermaltakeAIOTab();
}

QWidget* OpenRGBThermaltakeAIOPlugin::GetWidget()
{
    return(ui);
}

QMenu* OpenRGBThermaltakeAIOPlugin::GetTrayMenu()
{
    return(nullptr);
}

void OpenRGBThermaltakeAIOPlugin::Unload()
{
}

void OpenRGBThermaltakeAIOPlugin::OnProfileAboutToLoad()
{
}

void OpenRGBThermaltakeAIOPlugin::OnProfileLoad(nlohmann::json /*profile_data*/)
{
}

nlohmann::json OpenRGBThermaltakeAIOPlugin::OnProfileSave()
{
    return(nlohmann::json());
}

unsigned char* OpenRGBThermaltakeAIOPlugin::OnSDKCommand(unsigned int /*pkt_id*/, unsigned char* /*data*/, unsigned int* data_size)
{
    *data_size = 0;
    return(nullptr);
}

/*-----------------------------------------------------*\
| Update Signals                                        |
\*-----------------------------------------------------*/
void OpenRGBThermaltakeAIOPlugin::ProfileManagerUpdated(unsigned int /*update_reason*/)
{
}

void OpenRGBThermaltakeAIOPlugin::ResourceManagerUpdated(unsigned int /*update_reason*/)
{
}

void OpenRGBThermaltakeAIOPlugin::SettingsManagerUpdated(unsigned int /*update_reason*/)
{
}
