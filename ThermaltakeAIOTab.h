/*---------------------------------------------------------*\
| ThermaltakeAIOTab.h                                        |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <QWidget>
#include <QColor>
#include <nlohmann/json.hpp>
#include "ThermaltakeAIODevice.h"

namespace Ui
{
    class ThermaltakeAIOTab;
}

class ThermaltakeAIOMprisWatcher;

class ThermaltakeAIOTab : public QWidget
{
    Q_OBJECT

public:
    explicit ThermaltakeAIOTab(QWidget* parent = nullptr);
    ~ThermaltakeAIOTab();

    /*-----------------------------------------------------*\
    | OpenRGB Profile save/restore. SaveProfileData() is     |
    | returned as-is from the plugin's OnProfileSave() (the   |
    | host keys it under the plugin's name in the profile      |
    | file); LoadProfileData() is handed straight back via      |
    | OnProfileLoad() on the matching profile_data[Name]           |
    | sub-object (null if this plugin wasn't in that profile).      |
    \*-----------------------------------------------------*/
    nlohmann::json      SaveProfileData() const;
    void                LoadProfileData(const nlohmann::json& data);

private slots:
    void on_choose_image_button_clicked();
    void on_blank_canvas_button_clicked();
    void on_set_standby_button_clicked();
    void on_background_color_button_clicked();
    void on_start_stop_button_clicked();
    void on_visualisation_combo_currentIndexChanged(int index);
    void on_radial_off_radio_toggled(bool checked);
    void on_radial_temp_radio_toggled(bool checked);
    void on_radial_util_radio_toggled(bool checked);
    void on_cpu_color_button_clicked();
    void on_gpu_color_button_clicked();
    void on_ram_color_button_clicked();
    void on_music_text_color_button_clicked();
    void on_music_show_details_checkbox_toggled(bool checked);
    void on_music_field_title_checkbox_toggled(bool checked);
    void on_music_field_artist_checkbox_toggled(bool checked);
    void on_music_field_album_checkbox_toggled(bool checked);
    void on_music_text_size_edit_editingFinished();
    void on_music_outer_diameter_edit_editingFinished();
    void on_music_inner_diameter_edit_editingFinished();
    void on_debug_frame_index_checkbox_toggled(bool checked);
    void on_fps_slider_valueChanged(int value);
    void on_brightness_slider_valueChanged(int value);
    void RefreshConnectionStatus();

    /* jellyfin-tui's now-playing metadata, via ThermaltakeAIOMprisWatcher's MPRIS/D-Bus watch. */
    void OnNowPlayingChanged(const QString& art_path, const QString& title, const QString& artist, const QString& album, bool has_track);

private:
    Ui::ThermaltakeAIOTab* ui;
    ThermaltakeAIODevice*  device;
    QString                current_image_path;
    QColor                 background_color;

    /*-----------------------------------------------------*\
    | True once anything streamable has been set on the      |
    | device -- an image/GIF file OR a blank canvas. Used to  |
    | gate the Start button instead of current_image_path     |
    | emptiness, which is false for the blank canvas (it has   |
    | content but no file path) and so wrongly re-disabled     |
    | the button on the next connection-status tick.           |
    \*-----------------------------------------------------*/
    bool                   has_streamable_content = false;

    /*-----------------------------------------------------*\
    | Set by LoadProfileData() when the saved profile had     |
    | streaming on. The panel is often not connected yet at    |
    | the moment a profile loads (plugin/device init race on    |
    | OpenRGB startup), so this is consumed by TryAutoStart(),   |
    | which LoadProfileData() calls immediately and which        |
    | RefreshConnectionStatus() also retries on its 3s timer      |
    | until it succeeds. Cleared once consumed, or as soon as      |
    | the user manually starts/stops streaming themselves.          |
    \*-----------------------------------------------------*/
    bool                   pending_auto_start = false;

    /* Per-module gauge colours, shared across the temperature and utilisation views. */
    QColor                 module_color_cpu;
    QColor                 module_color_gpu;
    QColor                 module_color_ram;

    /*-----------------------------------------------------*\
    | Music Visualiser state. now_playing_* mirrors whatever  |
    | ThermaltakeAIOMprisWatcher last reported for jellyfin-    |
    | tui; its art is streamed through the SAME composite/       |
    | crop/scale pipeline as a manually-picked image, but NOT  |
    | through current_image_path -- it's live/transient and     |
    | shouldn't be persisted into an OpenRGB Profile the way a   |
    | user's deliberately-chosen static image is.                 |
    \*-----------------------------------------------------*/
    ThermaltakeAIOMprisWatcher* mpris_watcher = nullptr;
    QColor                       music_text_color;
    QString                      now_playing_art_path;
    QString                      now_playing_title;
    QString                      now_playing_artist;
    QString                      now_playing_album;
    bool                         now_playing_has_track = false;

    void LoadAndStreamImage(const QString& path);
    void UpdateBackgroundColorButtonSwatch();
    void UpdateStartStopButtonLabel();
    void TryAutoStart();

    void PickModuleColor(QColor* target, const QString& module_name);
    void UpdateModuleColorSwatch(class QPushButton* button, const QColor& color);
    void UpdateModuleColorSwatches();

    QVector<QImage> CompositeImagesFromPath(const QString& path) const;
    void            StreamNowPlayingArt(const QString& path);
    void            UpdateMusicTextColorButtonSwatch();
    void            UpdateMusicScrollTextFromNowPlaying();
    void            UpdateNowPlayingLabel();

    /*-----------------------------------------------------*\
    | Music Visualiser sizing fields (text size, outer/inner  |
    | diameter) are free-text QLineEdits rather than sliders,  |
    | per the user's request for fine-tunable exact values --   |
    | ApplyMusicSizing() clamps whatever the three fields         |
    | currently contain to sane, mutually-consistent ranges          |
    | (inner always kept below outer), writes the clamped               |
    | value back into each field so out-of-range input visibly            |
    | snaps to what was actually applied, and pushes the result             |
    | to the device. Shared by all three editingFinished slots  |
    | above since the fields constrain each other.               |
    \*-----------------------------------------------------*/
    static constexpr int MUSIC_TEXT_SIZE_MIN = 8;
    static constexpr int MUSIC_TEXT_SIZE_MAX = 150;
    static constexpr int MUSIC_DIAMETER_MIN  = 0;
    static constexpr int MUSIC_DIAMETER_MAX  = 100;

    void ApplyMusicSizing();
};
