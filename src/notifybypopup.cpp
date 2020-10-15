/*
    SPDX-FileCopyrightText: 2005-2009 Olivier Goffart <ogoffart at kde.org>
    SPDX-FileCopyrightText: 2008 Dmitry Suzdalev <dimsuz@gmail.com>
    SPDX-FileCopyrightText: 2014 Martin Klapetek <mklapetek@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "notifybypopup.h"

#include "imageconverter.h"
#include "knotifyconfig.h"
#include "knotification.h"
#include "notifications_interface.h"
#include "debug_p.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QDBusConnection>
#include <QHash>
#include <QPointer>
#include <QMutableListIterator>
#include <QUrl>

#include <KConfigGroup>

class NotifyByPopupPrivate {
public:
    NotifyByPopupPrivate(NotifyByPopup *parent)
        : dbusInterface(QStringLiteral("org.freedesktop.Notifications"),
                        QStringLiteral("/org/freedesktop/Notifications"),
                        QDBusConnection::sessionBus())
        , q(parent) {}

    /**
     * Sends notification to DBus "org.freedesktop.notifications" interface.
     * @param id knotify-sid identifier of notification
     * @param config notification data
     * @param update If true, will request the DBus service to update
                     the notification with new data from \c notification
     *               Otherwise will put new notification on screen
     * @return true for success or false if there was an error.
     */
    bool sendNotificationToServer(KNotification *notification, const KNotifyConfig &config, bool update = false);

    /**
     * Find the caption and the icon name of the application
     */
    void getAppCaptionAndIconName(const KNotifyConfig &config, QString *appCaption, QString *iconName);
    /*
     * Query the dbus server for notification capabilities
     * If no DBus server is present, use fallback capabilities for KPassivePopup
     */
    void queryPopupServerCapabilities();

    /**
     * DBus notification daemon capabilities cache.
     * Do not use this variable. Use #popupServerCapabilities() instead.
     * @see popupServerCapabilities
     */
    QStringList popupServerCapabilities;

    /**
     * In case we still don't know notification server capabilities,
     * we need to query those first. That's done in an async way
     * so we queue all notifications while waiting for the capabilities
     * to return, then process them from this queue
     */
    QList<QPair<KNotification*, KNotifyConfig> > notificationQueue;
    /**
     * Whether the DBus notification daemon capability cache is up-to-date.
     */
    bool dbusServiceCapCacheDirty;

    /*
     * As we communicate with the notification server over dbus
     * we use only ids, this is for fast KNotifications lookup
     */
    QHash<uint, QPointer<KNotification>> notifications;

    org::freedesktop::Notifications dbusInterface;

    NotifyByPopup * const q;
};

//---------------------------------------------------------------------------------------

NotifyByPopup::NotifyByPopup(QObject *parent)
  : KNotificationPlugin(parent),
    d(new NotifyByPopupPrivate(this))
{
    d->dbusServiceCapCacheDirty = true;

    connect(&d->dbusInterface, &org::freedesktop::Notifications::ActionInvoked, this, &NotifyByPopup::onNotificationActionInvoked);

    connect(&d->dbusInterface, &org::freedesktop::Notifications::NotificationClosed, this, &NotifyByPopup::onNotificationClosed);
}


NotifyByPopup::~NotifyByPopup()
{
    delete d;
}

void NotifyByPopup::notify(KNotification *notification, KNotifyConfig *notifyConfig)
{
    notify(notification, *notifyConfig);
}

void NotifyByPopup::notify(KNotification *notification, const KNotifyConfig &notifyConfig)
{
    if (d->notifications.contains(notification->id())) {
        // notification is already on the screen, do nothing
        finish(notification);
        return;
    }

    if (d->dbusServiceCapCacheDirty) {
        // if we don't have the server capabilities yet, we need to query for them first;
        // as that is an async dbus operation, we enqueue the notification and process them
        // when we receive dbus reply with the server capabilities
        d->notificationQueue.append(qMakePair(notification, notifyConfig));
        d->queryPopupServerCapabilities();
    } else {
        if (!d->sendNotificationToServer(notification, notifyConfig)) {
            finish(notification); //an error occurred.
        }
    }
}

void NotifyByPopup::update(KNotification *notification, KNotifyConfig *notifyConfig)
{
    update(notification, *notifyConfig);
}

void NotifyByPopup::update(KNotification *notification, const KNotifyConfig &notifyConfig)
{
    d->sendNotificationToServer(notification, notifyConfig, true);
}

void NotifyByPopup::close(KNotification *notification)
{
    uint id = d->notifications.key(notification, 0);

    if (id == 0) {
        qCDebug(LOG_KNOTIFICATIONS) << "not found dbus id to close" << notification->id();
        return;
    }

    d->dbusInterface.CloseNotification(id);

    QMutableListIterator<QPair<KNotification*, KNotifyConfig> > iter(d->notificationQueue);
    while (iter.hasNext()) {
        auto &item = iter.next();
        if (item.first == notification) {
            iter.remove();
        }
    }
}

void NotifyByPopup::onNotificationActionInvoked(uint notificationId, const QString &actionKey)
{
    auto iter = d->notifications.find(notificationId);
    if (iter == d->notifications.end()) {
        return;
    }

    KNotification *n = *iter;
    if (n) {
        if (actionKey == QLatin1String("default") && !n->defaultAction().isEmpty()) {
            emit actionInvoked(n->id(), 0);
        } else {
            bool ok;
            const int actionIndex = actionKey.toInt(&ok);

            if (!ok || actionIndex < 1 || actionIndex > n->actions().size()) {
                qCWarning(LOG_KNOTIFICATIONS) << "Invalid action key" << actionKey;
            }

            emit actionInvoked(n->id(), actionIndex);
        }
    } else {
        d->notifications.erase(iter);
    }
}

void NotifyByPopup::onNotificationClosed(uint dbus_id, uint reason)
{
    auto iter = d->notifications.find(dbus_id);
    if (iter == d->notifications.end()) {
        return;
    }
    KNotification *n = *iter;
    d->notifications.remove(dbus_id);

    if (n) {
        emit finished(n);
        // The popup bubble is the only user facing part of a notification,
        // if the user closes the popup, it means he wants to get rid
        // of the notification completely, including playing sound etc
        // Therefore we close the KNotification completely after closing
        // the popup, but only if the reason is 2, which means "user closed"
        if (reason == 2) {
            n->close();
        }
    }
}

void NotifyByPopupPrivate::getAppCaptionAndIconName(const KNotifyConfig &notifyConfig, QString *appCaption, QString *iconName)
{
    KConfigGroup globalgroup(&(*notifyConfig.eventsfile), QStringLiteral("Global"));
    *appCaption = globalgroup.readEntry("Name", globalgroup.readEntry("Comment", notifyConfig.appname));

    KConfigGroup eventGroup(&(*notifyConfig.eventsfile), QStringLiteral("Event/%1").arg(notifyConfig.eventid));
    if (eventGroup.hasKey("IconName")) {
        *iconName = eventGroup.readEntry("IconName", notifyConfig.appname);
    } else {
        *iconName = globalgroup.readEntry("IconName", notifyConfig.appname);
    }
}

bool NotifyByPopupPrivate::sendNotificationToServer(KNotification *notification, const KNotifyConfig &notifyConfig_nocheck, bool update)
{
    uint updateId = notifications.key(notification, 0);

    if (update) {
        if (updateId == 0) {
            // we have nothing to update; the notification we're trying to update
            // has been already closed
            return false;
        }
    }

    QString appCaption;
    QString iconName;
    getAppCaptionAndIconName(notifyConfig_nocheck, &appCaption, &iconName);

    //did the user override the icon name?
    if (!notification->iconName().isEmpty()) {
        iconName = notification->iconName();
    }

    QString title = notification->title().isEmpty() ? appCaption : notification->title();
    QString text = notification->text();

    if (!popupServerCapabilities.contains(QLatin1String("body-markup"))) {
        title = q->stripRichText(title);
        text = q->stripRichText(text);
    }

    // freedesktop.org spec defines action list to be list like
    // (act_id1, action1, act_id2, action2, ...)
    //
    // assign id's to actions like it's done in fillPopup() method
    // (i.e. starting from 1)
    QStringList actionList;
    if (popupServerCapabilities.contains(QLatin1String("actions"))) {
        QString defaultAction = notification->defaultAction();
        if (!defaultAction.isEmpty()) {
            actionList.append(QStringLiteral("default"));
            actionList.append(defaultAction);
        }
        int actId = 0;
        const auto listActions = notification->actions();
        for (const QString &actionName : listActions) {
            actId++;
            actionList.append(QString::number(actId));
            actionList.append(actionName);
        }
    }

    QVariantMap hintsMap;
    // Add the application name to the hints.
    // According to freedesktop.org spec, the app_name is supposed to be the application's "pretty name"
    // but in some places it's handy to know the application name itself
    if (!notification->appName().isEmpty()) {
        hintsMap[QStringLiteral("x-kde-appname")] = notification->appName();
    }

    if (!notification->eventId().isEmpty()) {
        hintsMap[QStringLiteral("x-kde-eventId")] = notification->eventId();
    }

    if (notification->flags() & KNotification::SkipGrouping) {
        hintsMap[QStringLiteral("x-kde-skipGrouping")] = 1;
    }

    if (!(notification->flags() & KNotification::Persistent)) {
        hintsMap[QStringLiteral("transient")] = true;
    }

    QString desktopFileName = QGuiApplication::desktopFileName();
    if (!desktopFileName.isEmpty()) {
        // handle apps which set the desktopFileName property with filename suffix,
        // due to unclear API dox (https://bugreports.qt.io/browse/QTBUG-75521)
        if (desktopFileName.endsWith(QLatin1String(".desktop"))) {
            desktopFileName.chop(8);
        }
        hintsMap[QStringLiteral("desktop-entry")] = desktopFileName;
    }

    int urgency = -1;
    switch (notification->urgency()) {
    case KNotification::DefaultUrgency:
        break;
    case KNotification::LowUrgency:
        urgency = 0;
        break;
    case KNotification::NormalUrgency:
        Q_FALLTHROUGH();
    // freedesktop.org notifications only know low, normal, critical
    case KNotification::HighUrgency:
        urgency = 1;
        break;
    case KNotification::CriticalUrgency:
        urgency = 2;
        break;
    }

    if (urgency > -1) {
        hintsMap[QStringLiteral("urgency")] = urgency;
    }

    const QVariantMap hints = notification->hints();
    for (auto it = hints.constBegin(); it != hints.constEnd(); ++it) {
        hintsMap[it.key()] = it.value();
    }

    //FIXME - reenable/fix
    // let's see if we've got an image, and store the image in the hints map
    if (!notification->pixmap().isNull()) {
        QByteArray pixmapData;
        QBuffer buffer(&pixmapData);
        buffer.open(QIODevice::WriteOnly);
        notification->pixmap().save(&buffer, "PNG");
        buffer.close();
        hintsMap[QStringLiteral("image_data")] = ImageConverter::variantForImage(QImage::fromData(pixmapData));
    }

    // Persistent     => 0  == infinite timeout
    // CloseOnTimeout => -1 == let the server decide
    int timeout = (notification->flags() & KNotification::Persistent) ? 0 : -1;

    const QDBusPendingReply<uint> reply = dbusInterface.Notify(appCaption, updateId, iconName, title, text, actionList, hintsMap, timeout);

    //parent is set to the notification so that no-one ever accesses a dangling pointer on the notificationObject property
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, notification);

    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, q, [this, notification](QDBusPendingCallWatcher *watcher){
        watcher->deleteLater();
        QDBusPendingReply<uint> reply = *watcher;
        notifications.insert(reply.argumentAt<0>(), notification);
    });

    return true;
}

void NotifyByPopupPrivate::queryPopupServerCapabilities()
{
    if (!dbusServiceCapCacheDirty) {
        return;
    }

    QDBusPendingReply<QStringList> call = dbusInterface.GetCapabilities();

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call);

    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, q, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        const QDBusPendingReply<QStringList> reply = *watcher;
        const QStringList capabilities = reply.argumentAt<0>();
        popupServerCapabilities = capabilities;
        dbusServiceCapCacheDirty = false;

        // re-run notify() on all enqueued notifications
        for (const QPair<KNotification*, KNotifyConfig> &noti : qAsConst(notificationQueue)) {
            q->notify(noti.first, noti.second);
        }

        notificationQueue.clear();
    });
}
