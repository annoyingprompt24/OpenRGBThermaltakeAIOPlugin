/*---------------------------------------------------------*\
| ThermaltakeAIOMprisWatcher.h                                |
|                                                             |
|   Watches the jellyfin-tui MPRIS media player over the      |
|   session D-Bus for now-playing changes (art/title/artist/  |
|   album), for the Music Visualiser panel. Scoped to          |
|   jellyfin-tui specifically (fixed bus name) rather than      |
|   generic "any MPRIS player" support.                          |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class ThermaltakeAIOMprisWatcher : public QObject
{
    Q_OBJECT

public:
    explicit ThermaltakeAIOMprisWatcher(QObject* parent = nullptr);

signals:
    /*-----------------------------------------------------*\
    | art_path is a local filesystem path (empty if the      |
    | current track has no local art, or nothing is playing). |
    | has_track is false when jellyfin-tui isn't running or     |
    | has no current track at all -- callers should hold the      |
    | last-known frame rather than treating this as "clear".        \*-----------------------------------------------------*/
    void NowPlayingChanged(const QString& art_path, const QString& title, const QString& artist, const QString& album, bool has_track);

private slots:
    void OnServiceOwnerChanged(const QString& name, const QString& old_owner, const QString& new_owner);
    void OnPropertiesChanged(const QString& interface_name, const QVariantMap& changed_properties, const QStringList& invalidated_properties);

private:
    static constexpr const char* SERVICE  = "org.mpris.MediaPlayer2.jellyfin-tui";
    static constexpr const char* OBJPATH  = "/org/mpris/MediaPlayer2";
    static constexpr const char* IFACE    = "org.mpris.MediaPlayer2.Player";

    void RefreshFromService();
    void EmitFromMetadata(const QVariantMap& metadata);
};
