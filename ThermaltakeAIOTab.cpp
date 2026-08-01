/*---------------------------------------------------------*\
| ThermaltakeAIOTab.cpp                                       |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOTab.h"
#include "ui_ThermaltakeAIOTab.h"
#include "ThermaltakeAIOMprisWatcher.h"

#include <QFileDialog>
#include <QColorDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QIntValidator>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QTimer>
#include <QPixmap>

ThermaltakeAIOTab::ThermaltakeAIOTab(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::ThermaltakeAIOTab),
    background_color(Qt::black),
    /* Defaults must match ThermaltakeAIODevice's own initial module colours. */
    module_color_cpu(255, 99, 71),
    module_color_gpu(94, 214, 108),
    module_color_ram(84, 170, 255),
    /* Matches ThermaltakeAIODevice's own initial music text colour. */
    music_text_color(94, 214, 108)
{
    ui->setupUi(this);

    device = new ThermaltakeAIODevice();

    /*-----------------------------------------------------*\
    | Visualisation selector. Index 0 = Radial Gauge, 1 =     |
    | Music Visualiser -- on_visualisation_combo_current-      |
    | IndexChanged() below relies on that fixed order.          |
    \*-----------------------------------------------------*/
    ui->visualisation_combo->addItem("Radial Gauge");
    ui->visualisation_combo->addItem("Music Visualiser");

    /* Reflect whatever target fps the device actually started with (default, or env override). */
    int initial_fps = device->GetTargetFps();
    ui->fps_slider->setValue(initial_fps);
    ui->fps_slider_label->setText(QString("Target FPS: %1").arg(initial_fps));

    int initial_brightness = device->GetBrightness();
    ui->brightness_slider->setValue(initial_brightness);
    ui->brightness_slider_label->setText(QString("Brightness: %1%").arg(initial_brightness));

    device->SetModuleColors(module_color_cpu, module_color_gpu, module_color_ram);
    device->SetMusicBackgroundColor(background_color);
    device->SetMusicTextColor(music_text_color);
    device->SetMusicShowDetails(ui->music_show_details_checkbox->isChecked());

    /*-----------------------------------------------------*\
    | Free-text sizing fields rather than sliders, per the    |
    | user's request for exact fine-tunable values. Default    |
    | text mirrors ThermaltakeAIODevice's own construction-      |
    | time defaults; ApplyMusicSizing() (also called once here)   |
    | is what actually clamps/pushes them to the device, so   |
    | these starting values just need to already be in-range.  |
    \*-----------------------------------------------------*/
    ui->music_text_size_edit->setValidator(new QIntValidator(MUSIC_TEXT_SIZE_MIN, MUSIC_TEXT_SIZE_MAX, this));
    ui->music_outer_diameter_edit->setValidator(new QIntValidator(MUSIC_DIAMETER_MIN, MUSIC_DIAMETER_MAX, this));
    ui->music_inner_diameter_edit->setValidator(new QIntValidator(MUSIC_DIAMETER_MIN, MUSIC_DIAMETER_MAX, this));
    ui->music_text_size_edit->setText("46");
    ui->music_outer_diameter_edit->setText("94");
    ui->music_inner_diameter_edit->setText("62");
    ApplyMusicSizing();

    UpdateBackgroundColorButtonSwatch();
    UpdateModuleColorSwatches();
    UpdateMusicTextColorButtonSwatch();
    UpdateNowPlayingLabel();
    RefreshConnectionStatus();

    /*-----------------------------------------------------*\
    | Re-check connection periodically so the tab notices    |
    | the panel being plugged in/out without a manual        |
    | refresh action.                                        |
    \*-----------------------------------------------------*/
    QTimer* connection_timer = new QTimer(this);
    connect(connection_timer, &QTimer::timeout, this, &ThermaltakeAIOTab::RefreshConnectionStatus);
    connection_timer->start(3000);

    /*-----------------------------------------------------*\
    | jellyfin-tui now-playing watcher for the Music          |
    | Visualiser panel. Runs regardless of which visualisation |
    | is currently selected, so switching into Music Visualiser  |
    | mode has an already-current now_playing_* to show       |
    | immediately instead of waiting for the next track change. |
    \*-----------------------------------------------------*/
    mpris_watcher = new ThermaltakeAIOMprisWatcher(this);
    connect(mpris_watcher, &ThermaltakeAIOMprisWatcher::NowPlayingChanged,
            this, &ThermaltakeAIOTab::OnNowPlayingChanged);
}

ThermaltakeAIOTab::~ThermaltakeAIOTab()
{
    device->Stop();
    delete device;
    delete ui;
}

void ThermaltakeAIOTab::RefreshConnectionStatus()
{
    if(!device->IsConnected())
    {
        device->Open();
    }

    if(device->IsConnected())
    {
        ui->connection_status_label->setText("Panel connected");
        ui->start_stop_button->setEnabled(has_streamable_content);
        /* Standby is a self-contained flash write -- it picks its own image, so it only needs a connected panel. */
        ui->set_standby_button->setEnabled(true);
    }
    else
    {
        ui->connection_status_label->setText("Panel not connected");
        ui->start_stop_button->setEnabled(false);
        ui->set_standby_button->setEnabled(false);
    }

    TryAutoStart();
}

void ThermaltakeAIOTab::TryAutoStart()
{
    if(!pending_auto_start)
    {
        return;
    }

    /* Content and connection may both still be catching up right after a profile load -- keep retrying via the connection timer until both are ready. */
    if(!has_streamable_content || !device->IsConnected())
    {
        return;
    }

    if(!device->IsStreaming())
    {
        device->Start();
        UpdateStartStopButtonLabel();
    }

    pending_auto_start = false;
}

void ThermaltakeAIOTab::on_choose_image_button_clicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Choose Image", QString(),
                                                  "Images (*.png *.jpg *.jpeg *.bmp *.gif)");
    if(path.isEmpty())
    {
        return;
    }

    LoadAndStreamImage(path);
}

void ThermaltakeAIOTab::on_blank_canvas_button_clicked()
{
    QImage blank(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE, QImage::Format_RGB32);
    blank.fill(Qt::black);

    QVector<QImage> single_image;
    single_image.push_back(blank);
    device->SetImages(single_image);

    /* Not a file -- clear so the background-color re-composite path doesn't try to reload one. */
    current_image_path.clear();
    has_streamable_content = true;

    ui->preview_label->setPixmap(QPixmap::fromImage(blank).scaled(ui->preview_label->size(),
                                                                     Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->start_stop_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::on_set_standby_button_clicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Choose Standby Image", QString(),
                                                  "Images (*.png *.jpg *.jpeg *.bmp *.gif)");
    if(path.isEmpty())
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Prepare the frame exactly like the live-stream path:  |
    | composite onto the background color first (the panel   |
    | is a plain JPEG raster with no transparency), then     |
    | center-crop to square and scale to the panel size. For |
    | an animated source we just take the first frame.       |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if(image.isNull())
    {
        ui->standby_status_label->setText("Failed to load image");
        return;
    }

    QImage composited(image.size(), QImage::Format_RGB32);
    composited.fill(background_color);
    QPainter painter(&composited);
    painter.drawImage(0, 0, image);
    painter.end();

    int   side = qMin(composited.width(), composited.height());
    QRect crop_rect((composited.width() - side) / 2, (composited.height() - side) / 2, side, side);
    QImage square = composited.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    /*-----------------------------------------------------*\
    | The flash write blocks ~200ms. Show status and force   |
    | an immediate repaint so the label updates before the    |
    | UI thread stalls, then disable the button for the       |
    | duration to prevent re-entrancy.                        |
    \*-----------------------------------------------------*/
    ui->set_standby_button->setEnabled(false);
    ui->standby_status_label->setText("Writing standby image...");
    ui->standby_status_label->repaint();

    bool ok = device->SetStandbyImage(square);

    ui->standby_status_label->setText(ok ? "Standby image saved to panel" : "Standby write failed");
    ui->set_standby_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::on_background_color_button_clicked()
{
    QColor chosen = QColorDialog::getColor(background_color, this, "Background Color for Transparent Pixels");
    if(!chosen.isValid())
    {
        return;
    }

    background_color = chosen;
    UpdateBackgroundColorButtonSwatch();

    /* The Music Visualiser's border band always matches this colour -- see ThermaltakeAIODevice::DrawMusicVisualiser. */
    device->SetMusicBackgroundColor(background_color);

    /* Re-composite the already-loaded image against the new background, if any. */
    if(!current_image_path.isEmpty())
    {
        LoadAndStreamImage(current_image_path);
    }
}

QVector<QImage> ThermaltakeAIOTab::CompositeImagesFromPath(const QString& path) const
{
    /*-----------------------------------------------------*\
    | QImageReader::read() auto-advances to the next frame  |
    | on its own for animated formats like GIF -- no        |
    | jumpToNextImage() needed (and combining the two just  |
    | double-advances, or the plugin refuses the sequence,  |
    | which is why an earlier version of this only ever     |
    | captured frame 0). For a plain static image, read()   |
    | just returns one frame then a null image next call.   |
    \*-----------------------------------------------------*/
    QImageReader reader(path);
    reader.setAutoTransform(true);

    QVector<QImage> loaded_images;
    QImage image;

    while(!(image = reader.read()).isNull())
    {
        /*-------------------------------------------------*\
        | Composite onto a solid background BEFORE cropping/ |
        | scaling, not after -- the panel has no concept of   |
        | transparency (it's a plain JPEG raster), and GIF     |
        | encoders routinely leave undefined/garbage RGB data  |
        | under fully-transparent pixels since it's never       |
        | meant to be shown. Compositing first also avoids      |
        | smooth-scaling blending that garbage color into        |
        | semi-transparent edge pixels.                           |
        \*-------------------------------------------------*/
        QImage composited(image.size(), QImage::Format_RGB32);
        composited.fill(background_color);
        QPainter painter(&composited);
        painter.drawImage(0, 0, image);
        painter.end();

        int side = qMin(composited.width(), composited.height());
        QRect crop_rect((composited.width() - side) / 2, (composited.height() - side) / 2, side, side);
        QImage square = composited.copy(crop_rect).scaled(ThermaltakeAIODevice::PANEL_SIZE, ThermaltakeAIODevice::PANEL_SIZE,
                                                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        loaded_images.push_back(square);
    }

    return(loaded_images);
}

void ThermaltakeAIOTab::LoadAndStreamImage(const QString& path)
{
    QVector<QImage> loaded_images = CompositeImagesFromPath(path);

    if(loaded_images.isEmpty())
    {
        ui->connection_status_label->setText("Failed to load image");
        return;
    }

    device->SetImages(loaded_images);

    current_image_path     = path;
    has_streamable_content = true;
    ui->preview_label->setPixmap(QPixmap::fromImage(loaded_images.first()).scaled(ui->preview_label->size(),
                                                                                    Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->start_stop_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::StreamNowPlayingArt(const QString& path)
{
    /*-----------------------------------------------------*\
    | Same pipeline as LoadAndStreamImage(), but deliberately |
    | does NOT touch current_image_path -- this content is     |
    | live/transient (jellyfin-tui's current album art), not     |
    | something the user picked to persist in an OpenRGB Profile. \*-----------------------------------------------------*/
    QVector<QImage> loaded_images = CompositeImagesFromPath(path);

    if(loaded_images.isEmpty())
    {
        return;
    }

    device->SetImages(loaded_images);

    has_streamable_content = true;
    ui->preview_label->setPixmap(QPixmap::fromImage(loaded_images.first()).scaled(ui->preview_label->size(),
                                                                                    Qt::KeepAspectRatio, Qt::SmoothTransformation));
    ui->start_stop_button->setEnabled(device->IsConnected());
}

void ThermaltakeAIOTab::UpdateBackgroundColorButtonSwatch()
{
    QString text_color = (background_color.lightness() > 128) ? "black" : "white";
    ui->background_color_button->setStyleSheet(
        QString("background-color: %1; color: %2;").arg(background_color.name(), text_color));
}

void ThermaltakeAIOTab::on_start_stop_button_clicked()
{
    /* A manual click always wins over any still-pending auto-start from a profile load. */
    pending_auto_start = false;

    if(device->IsStreaming())
    {
        device->Stop();
    }
    else
    {
        device->Start();
    }

    UpdateStartStopButtonLabel();
}

void ThermaltakeAIOTab::on_visualisation_combo_currentIndexChanged(int index)
{
    /* Index 0 = Radial Gauge, 1 = Music Visualiser -- matches the fixed add order in the constructor. */
    bool music_mode = (index == 1);

    ui->radial_gauge_panel->setVisible(!music_mode);
    ui->music_visualiser_panel->setVisible(music_mode);

    device->SetVisualisationMode(music_mode ? ThermaltakeAIODevice::VisualisationMode::MusicVisualiser
                                             : ThermaltakeAIODevice::VisualisationMode::RadialGauge);

    if(music_mode)
    {
        /* Immediately reflect whatever jellyfin-tui already has loaded, rather than waiting for its next track change. */
        UpdateMusicScrollTextFromNowPlaying();

        if(!now_playing_art_path.isEmpty())
        {
            StreamNowPlayingArt(now_playing_art_path);
        }
    }
}

void ThermaltakeAIOTab::on_radial_off_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Off);
    }
}

void ThermaltakeAIOTab::on_radial_temp_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Temperature);
    }
}

void ThermaltakeAIOTab::on_radial_util_radio_toggled(bool checked)
{
    if(checked)
    {
        device->SetOverlayMode(ThermaltakeAIODevice::OverlayMode::Utilization);
    }
}

void ThermaltakeAIOTab::on_cpu_color_button_clicked()
{
    PickModuleColor(&module_color_cpu, "CPU");
}

void ThermaltakeAIOTab::on_gpu_color_button_clicked()
{
    PickModuleColor(&module_color_gpu, "GPU");
}

void ThermaltakeAIOTab::on_ram_color_button_clicked()
{
    PickModuleColor(&module_color_ram, "RAM");
}

void ThermaltakeAIOTab::on_music_text_color_button_clicked()
{
    QColor chosen = QColorDialog::getColor(music_text_color, this, "Music Visualiser Text Colour");
    if(!chosen.isValid())
    {
        return;
    }

    music_text_color = chosen;
    UpdateMusicTextColorButtonSwatch();
    device->SetMusicTextColor(music_text_color);
}

void ThermaltakeAIOTab::on_music_show_details_checkbox_toggled(bool checked)
{
    device->SetMusicShowDetails(checked);
}

void ThermaltakeAIOTab::on_music_field_title_checkbox_toggled(bool /*checked*/)
{
    UpdateMusicScrollTextFromNowPlaying();
}

void ThermaltakeAIOTab::on_music_field_artist_checkbox_toggled(bool /*checked*/)
{
    UpdateMusicScrollTextFromNowPlaying();
}

void ThermaltakeAIOTab::on_music_field_album_checkbox_toggled(bool /*checked*/)
{
    UpdateMusicScrollTextFromNowPlaying();
}

void ThermaltakeAIOTab::on_music_text_size_edit_editingFinished()
{
    ApplyMusicSizing();
}

void ThermaltakeAIOTab::on_music_outer_diameter_edit_editingFinished()
{
    ApplyMusicSizing();
}

void ThermaltakeAIOTab::on_music_inner_diameter_edit_editingFinished()
{
    ApplyMusicSizing();
}

void ThermaltakeAIOTab::ApplyMusicSizing()
{
    int text_size = ui->music_text_size_edit->text().toInt();
    int outer_pct = ui->music_outer_diameter_edit->text().toInt();
    int inner_pct = ui->music_inner_diameter_edit->text().toInt();

    text_size = qBound(MUSIC_TEXT_SIZE_MIN, text_size, MUSIC_TEXT_SIZE_MAX);
    outer_pct = qBound(MUSIC_DIAMETER_MIN, outer_pct, MUSIC_DIAMETER_MAX);
    inner_pct = qBound(MUSIC_DIAMETER_MIN, inner_pct, MUSIC_DIAMETER_MAX);

    /* Inner must stay strictly below outer, or the band has zero/negative thickness. */
    if(inner_pct >= outer_pct)
    {
        inner_pct = qMax(MUSIC_DIAMETER_MIN, outer_pct - 1);
    }

    /* Write the clamped values back so out-of-range or conflicting input visibly snaps to what was actually applied. */
    ui->music_text_size_edit->setText(QString::number(text_size));
    ui->music_outer_diameter_edit->setText(QString::number(outer_pct));
    ui->music_inner_diameter_edit->setText(QString::number(inner_pct));

    device->SetMusicTextSizePx(text_size);
    device->SetMusicOuterDiameterPercent(outer_pct);
    device->SetMusicInnerDiameterPercent(inner_pct);
}

void ThermaltakeAIOTab::OnNowPlayingChanged(const QString& art_path, const QString& title, const QString& artist, const QString& album, bool has_track)
{
    now_playing_art_path  = art_path;
    now_playing_title      = title;
    now_playing_artist     = artist;
    now_playing_album      = album;
    now_playing_has_track  = has_track;

    UpdateNowPlayingLabel();

    bool music_mode = (ui->visualisation_combo->currentIndex() == 1);
    if(!music_mode)
    {
        return;
    }

    UpdateMusicScrollTextFromNowPlaying();

    if(!art_path.isEmpty())
    {
        StreamNowPlayingArt(art_path);
    }
}

void ThermaltakeAIOTab::UpdateMusicTextColorButtonSwatch()
{
    QString swatch_text_color = (music_text_color.lightness() > 128) ? "black" : "white";
    ui->music_text_color_button->setStyleSheet(
        QString("background-color: %1; color: %2;").arg(music_text_color.name(), swatch_text_color));
}

void ThermaltakeAIOTab::UpdateMusicScrollTextFromNowPlaying()
{
    /*-----------------------------------------------------*\
    | One field per revolution (Title, then Artist, then      |
    | Album, cycling) rather than all of them concatenated --  |
    | a joined string was too small/cramped to read on a        |
    | 480px round panel. The device owns which one is           |
    | currently showing (music_scroll_field_index); this just  |
    | supplies the ordered list of checked, non-empty fields.   |
    \*-----------------------------------------------------*/
    QStringList fields;

    if(ui->music_field_title_checkbox->isChecked() && !now_playing_title.isEmpty())
    {
        fields << now_playing_title;
    }
    if(ui->music_field_artist_checkbox->isChecked() && !now_playing_artist.isEmpty())
    {
        fields << now_playing_artist;
    }
    if(ui->music_field_album_checkbox->isChecked() && !now_playing_album.isEmpty())
    {
        fields << now_playing_album;
    }

    device->SetMusicScrollFields(fields);
}

void ThermaltakeAIOTab::UpdateNowPlayingLabel()
{
    if(!now_playing_has_track)
    {
        ui->music_now_playing_label->setText("jellyfin-tui not running / nothing playing");
        return;
    }

    QString label = now_playing_title.isEmpty() ? "(untitled track)" : now_playing_title;
    if(!now_playing_artist.isEmpty())
    {
        label += " -- " + now_playing_artist;
    }

    ui->music_now_playing_label->setText("Now playing: " + label);
}

void ThermaltakeAIOTab::PickModuleColor(QColor* target, const QString& module_name)
{
    QColor chosen = QColorDialog::getColor(*target, this, QString("%1 Gauge Color").arg(module_name));
    if(!chosen.isValid())
    {
        return;
    }

    *target = chosen;
    device->SetModuleColors(module_color_cpu, module_color_gpu, module_color_ram);
    UpdateModuleColorSwatches();
}

void ThermaltakeAIOTab::UpdateModuleColorSwatch(QPushButton* button, const QColor& color)
{
    QString text_color = (color.lightness() > 128) ? "black" : "white";
    button->setStyleSheet(QString("background-color: %1; color: %2;").arg(color.name(), text_color));
}

void ThermaltakeAIOTab::UpdateModuleColorSwatches()
{
    UpdateModuleColorSwatch(ui->cpu_color_button, module_color_cpu);
    UpdateModuleColorSwatch(ui->gpu_color_button, module_color_gpu);
    UpdateModuleColorSwatch(ui->ram_color_button, module_color_ram);
}

void ThermaltakeAIOTab::on_debug_frame_index_checkbox_toggled(bool checked)
{
    device->SetDebugFrameIndexEnabled(checked);
}

void ThermaltakeAIOTab::on_fps_slider_valueChanged(int value)
{
    device->SetTargetFps(value);
    ui->fps_slider_label->setText(QString("Target FPS: %1").arg(value));
}

void ThermaltakeAIOTab::on_brightness_slider_valueChanged(int value)
{
    device->SetBrightness(value);
    ui->brightness_slider_label->setText(QString("Brightness: %1%").arg(value));
}

void ThermaltakeAIOTab::UpdateStartStopButtonLabel()
{
    ui->start_stop_button->setText(device->IsStreaming() ? "Stop Streaming" : "Start Streaming");
}

nlohmann::json ThermaltakeAIOTab::SaveProfileData() const
{
    nlohmann::json data;

    /*-----------------------------------------------------*\
    | Blank Canvas leaves current_image_path empty despite   |
    | having streamable content, so it needs its own flag     |
    | to round-trip correctly (an empty image_path alone       |
    | would otherwise mean "nothing was ever set").              |
    \*-----------------------------------------------------*/
    data["image_path"]        = current_image_path.toStdString();
    data["blank_canvas"]      = has_streamable_content && current_image_path.isEmpty();
    data["background_color"]  = background_color.name().toStdString();
    data["module_color_cpu"]  = module_color_cpu.name().toStdString();
    data["module_color_gpu"]  = module_color_gpu.name().toStdString();
    data["module_color_ram"]  = module_color_ram.name().toStdString();
    data["target_fps"]        = device->GetTargetFps();
    data["brightness"]        = device->GetBrightness();
    data["debug_frame_index"] = ui->debug_frame_index_checkbox->isChecked();
    data["streaming"]         = device->IsStreaming();

    if(ui->radial_temp_radio->isChecked())
    {
        data["overlay_mode"] = "temperature";
    }
    else if(ui->radial_util_radio->isChecked())
    {
        data["overlay_mode"] = "utilization";
    }
    else
    {
        data["overlay_mode"] = "off";
    }

    data["visualisation_mode"] = (ui->visualisation_combo->currentIndex() == 1) ? "music_visualiser" : "radial_gauge";
    data["music_text_color"]   = music_text_color.name().toStdString();
    data["music_show_details"] = ui->music_show_details_checkbox->isChecked();
    data["music_field_title"]  = ui->music_field_title_checkbox->isChecked();
    data["music_field_artist"] = ui->music_field_artist_checkbox->isChecked();
    data["music_field_album"]  = ui->music_field_album_checkbox->isChecked();
    data["music_text_size_px"]        = ui->music_text_size_edit->text().toInt();
    data["music_outer_diameter_pct"]  = ui->music_outer_diameter_edit->text().toInt();
    data["music_inner_diameter_pct"]  = ui->music_inner_diameter_edit->text().toInt();

    return(data);
}

void ThermaltakeAIOTab::LoadProfileData(const nlohmann::json& data)
{
    /* profile_data[Name] is a null json (not an object) for a profile that predates this plugin, or never had this plugin active. Nothing to restore. */
    if(!data.is_object())
    {
        return;
    }

    background_color = QColor(QString::fromStdString(data.value("background_color", background_color.name().toStdString())));
    module_color_cpu = QColor(QString::fromStdString(data.value("module_color_cpu", module_color_cpu.name().toStdString())));
    module_color_gpu = QColor(QString::fromStdString(data.value("module_color_gpu", module_color_gpu.name().toStdString())));
    module_color_ram = QColor(QString::fromStdString(data.value("module_color_ram", module_color_ram.name().toStdString())));

    UpdateBackgroundColorButtonSwatch();
    device->SetModuleColors(module_color_cpu, module_color_gpu, module_color_ram);
    /* The Music Visualiser's border band always matches this colour -- see ThermaltakeAIODevice::DrawMusicVisualiser. */
    device->SetMusicBackgroundColor(background_color);
    UpdateModuleColorSwatches();

    /* Slider valueChanged slots push the value into the device and update their own labels. */
    ui->fps_slider->setValue(data.value("target_fps", device->GetTargetFps()));
    ui->brightness_slider->setValue(data.value("brightness", device->GetBrightness()));

    ui->debug_frame_index_checkbox->setChecked(data.value("debug_frame_index", false));

    std::string overlay_mode = data.value("overlay_mode", std::string("off"));
    if(overlay_mode == "temperature")
    {
        ui->radial_temp_radio->setChecked(true);
    }
    else if(overlay_mode == "utilization")
    {
        ui->radial_util_radio->setChecked(true);
    }
    else
    {
        ui->radial_off_radio->setChecked(true);
    }

    /*-----------------------------------------------------*\
    | Restore the actual image/blank-canvas content next, so |
    | the background color it composites against is already   |
    | up to date.                                               |
    \*-----------------------------------------------------*/
    bool        blank_canvas = data.value("blank_canvas", false);
    std::string image_path   = data.value("image_path", std::string());

    if(blank_canvas)
    {
        on_blank_canvas_button_clicked();
    }
    else if(!image_path.empty())
    {
        LoadAndStreamImage(QString::fromStdString(image_path));
    }

    /*-----------------------------------------------------*\
    | Music Visualiser settings. Text colour and the field     |
    | checkboxes are restored before the visualisation-combo    |
    | switch below, so if the saved mode was Music Visualiser,    |
    | switching into it immediately streams jellyfin-tui's art       |
    | (if any) with the right text colour/fields already in place.    |
    | The border band itself isn't saved separately -- it always       |
    | matches background_color, already restored above.                 |
    \*-----------------------------------------------------*/
    music_text_color = QColor(QString::fromStdString(data.value("music_text_color", music_text_color.name().toStdString())));
    UpdateMusicTextColorButtonSwatch();
    device->SetMusicTextColor(music_text_color);

    ui->music_show_details_checkbox->setChecked(data.value("music_show_details", true));
    ui->music_field_title_checkbox->setChecked(data.value("music_field_title", true));
    ui->music_field_artist_checkbox->setChecked(data.value("music_field_artist", true));
    ui->music_field_album_checkbox->setChecked(data.value("music_field_album", true));

    ui->music_text_size_edit->setText(QString::number(data.value("music_text_size_px", 46)));
    ui->music_outer_diameter_edit->setText(QString::number(data.value("music_outer_diameter_pct", 94)));
    ui->music_inner_diameter_edit->setText(QString::number(data.value("music_inner_diameter_pct", 62)));
    ApplyMusicSizing();

    std::string visualisation_mode = data.value("visualisation_mode", std::string("radial_gauge"));
    ui->visualisation_combo->setCurrentIndex(visualisation_mode == "music_visualiser" ? 1 : 0);

    /*-----------------------------------------------------*\
    | The panel is frequently not connected yet at the exact  |
    | moment a profile loads (device detection can still be    |
    | running during OpenRGB startup) -- TryAutoStart() here     |
    | catches the case where it's already connected, and the      |
    | RefreshConnectionStatus() timer retries otherwise.             |
    \*-----------------------------------------------------*/
    pending_auto_start = data.value("streaming", false);
    TryAutoStart();
}
