/*---------------------------------------------------------*\
| ThermaltakeAIOMprisWatcher.cpp                               |
|                                                             |
|   This file is part of the OpenRGB project                 |
|   SPDX-License-Identifier: GPL-2.0-or-later                |
\*---------------------------------------------------------*/

#include "ThermaltakeAIOMprisWatcher.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusArgument>
#include <QUrl>

ThermaltakeAIOMprisWatcher::ThermaltakeAIOMprisWatcher(QObject* parent) :
    QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    /* Fires when jellyfin-tui starts or exits, so we can (re)sync or fall back to "not playing". */
    bus.connect(QString(), "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                this, SLOT(OnServiceOwnerChanged(QString,QString,QString)));

    /* Fires on track change (and other property changes we ignore) while jellyfin-tui is running. */
    bus.connect(SERVICE, OBJPATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                this, SLOT(OnPropertiesChanged(QString,QVariantMap,QStringList)));

    RefreshFromService();
}

void ThermaltakeAIOMprisWatcher::OnServiceOwnerChanged(const QString& name, const QString& /*old_owner*/, const QString& new_owner)
{
    if(name != SERVICE)
    {
        return;
    }

    if(new_owner.isEmpty())
    {
        emit NowPlayingChanged(QString(), QString(), QString(), QString(), false);
    }
    else
    {
        RefreshFromService();
    }
}

void ThermaltakeAIOMprisWatcher::OnPropertiesChanged(const QString& interface_name, const QVariantMap& changed_properties, const QStringList& /*invalidated_properties*/)
{
    if(interface_name != IFACE || !changed_properties.contains("Metadata"))
    {
        return;
    }

    QVariant raw = changed_properties.value("Metadata");
    QVariantMap metadata;

    if(raw.canConvert<QDBusArgument>())
    {
        QDBusArgument arg = raw.value<QDBusArgument>();
        arg >> metadata;
    }
    else
    {
        metadata = raw.toMap();
    }

    EmitFromMetadata(metadata);
}

void ThermaltakeAIOMprisWatcher::RefreshFromService()
{
    QDBusInterface props_iface(SERVICE, OBJPATH, "org.freedesktop.DBus.Properties", QDBusConnection::sessionBus());

    if(!props_iface.isValid())
    {
        emit NowPlayingChanged(QString(), QString(), QString(), QString(), false);
        return;
    }

    QDBusReply<QVariant> reply = props_iface.call("Get", IFACE, "Metadata");

    if(!reply.isValid())
    {
        emit NowPlayingChanged(QString(), QString(), QString(), QString(), false);
        return;
    }

    QVariant raw = reply.value();
    QVariantMap metadata;

    if(raw.canConvert<QDBusArgument>())
    {
        QDBusArgument arg = raw.value<QDBusArgument>();
        arg >> metadata;
    }
    else
    {
        metadata = raw.toMap();
    }

    EmitFromMetadata(metadata);
}

void ThermaltakeAIOMprisWatcher::EmitFromMetadata(const QVariantMap& metadata)
{
    QString art_url = metadata.value("mpris:artUrl").toString();
    QUrl    art_uri(art_url);
    QString art_path = art_uri.isLocalFile() ? art_uri.toLocalFile() : QString();

    QString title = metadata.value("xesam:title").toString();
    QString album = metadata.value("xesam:album").toString();

    QVariant artist_variant = metadata.value("xesam:artist");
    QStringList artists;

    if(artist_variant.canConvert<QDBusArgument>())
    {
        QDBusArgument arg = artist_variant.value<QDBusArgument>();
        arg >> artists;
    }
    else
    {
        artists = artist_variant.toStringList();
    }

    QString artist    = artists.join(", ");
    bool    has_track = !title.isEmpty() || !art_path.isEmpty();

    emit NowPlayingChanged(art_path, title, artist, album, has_track);
}
