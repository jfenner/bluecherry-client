/*
 * Copyright 2010-2013 Bluecherry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "DVRServer.h"
#include "core/DVRCamera.h"
#include <QSettings>
#include <QNetworkRequest>
#include <QUrl>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslCertificate>
#include <QDebug>

DVRServer::DVRServer(int id, QObject *parent)
    : QObject(parent), configId(id), devicesLoaded(false)
{
    readFromSettings();

    api = new ServerRequestManager(this);

    connect(api, SIGNAL(loginSuccessful()), SLOT(updateCameras()));
    connect(api, SIGNAL(disconnected()), SLOT(disconnected()));

    if (m_autoConnect && !hostname().isEmpty() && !username().isEmpty())
        QTimer::singleShot(0, this, SLOT(login()));

    connect(&m_refreshTimer, SIGNAL(timeout()), SLOT(updateCameras()));
}

QVariant DVRServer::readSetting(const QString &key, const QVariant &def) const
{
    QSettings settings;
    return settings.value(QString::fromLatin1("servers/%1/").arg(configId).append(key), def);
}

void DVRServer::writeSetting(const QString &key, const QVariant &value)
{
    QSettings settings;
    settings.setValue(QString::fromLatin1("servers/%1/").arg(configId).append(key), value);

    emit changed();
}

void DVRServer::setDisplayName(const QString &name)
{
    if (m_displayName == name)
        return;

    m_displayName = name;
    writeSetting(QLatin1String("displayName"), name);
}

void DVRServer::setHostname(const QString &hostname)
{
    writeSetting(QLatin1String("hostname"), hostname);
    m_hostname = hostname;
}

void DVRServer::setPort(int port)
{
    writeSetting(QLatin1String("port"), port == 0 ? 7001 : port);
    m_port = port;
}

void DVRServer::setUsername(const QString &username)
{
    writeSetting(QLatin1String("username"), username);
    m_username = username;
}

void DVRServer::setPassword(const QString &password)
{
    writeSetting(QLatin1String("password"), password);
    m_password = password;
}

void DVRServer::setAutoConnect(bool autoConnect)
{
    writeSetting(QLatin1String("autoConnect"), autoConnect);
    m_autoConnect = autoConnect;
}

void DVRServer::removeServer()
{
    qDebug("Deleting DVR server %d", configId);

    emit serverRemoved(this);

    QSettings settings;
    settings.remove(QString::fromLatin1("servers/%1").arg(configId));

    deleteLater();
}

void DVRServer::login()
{
    api->login(username(), password());
}

void DVRServer::toggleOnline()
{
    if (api->isOnline())
        api->logout();
    else
        login();
}

void DVRServer::updateCameras()
{
    if (!api->isOnline())
    {
        m_refreshTimer.stop();
        return;
    }

    if (!m_refreshTimer.isActive())
        m_refreshTimer.start(60000);

    qDebug() << "DVRServer: Requesting cameras list";
    QNetworkReply *reply = api->sendRequest(QUrl(QLatin1String("/ajax/devices.php?XML=1")));
    connect(reply, SIGNAL(finished()), SLOT(updateCamerasReply()));

    reply = api->sendRequest(QUrl(QLatin1String("/ajax/stats.php")));
    connect(reply, SIGNAL(finished()), SLOT(updateStatsReply()));
}

void DVRServer::updateCamerasReply()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;

    qDebug() << "DVRServer: Received cameras list reply";

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        /* TODO: Handle this well */
        qWarning() << "DVRServer: Error from updating cameras:" << reply->errorString();
        return;
    }

    QByteArray data = reply->readAll();
    QXmlStreamReader xml(data);

    QSet<int> idSet;
    bool hasDevicesElement = false;
    bool wasEmpty = m_cameras.isEmpty();

    while (xml.readNextStartElement())
    {
        if (xml.name() == QLatin1String("devices"))
        {
            hasDevicesElement = true;

            while (xml.readNext() != QXmlStreamReader::Invalid)
            {
                if (xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == QLatin1String("devices"))
                    break;
                else if (xml.tokenType() != QXmlStreamReader::StartElement)
                    continue;

                if (xml.name() == QLatin1String("device"))
                {
                    bool ok = false;
                    QString idv = xml.attributes().value(QLatin1String("id")).toString();
                    if (idv.isNull())
                        continue;
                    int deviceId = (int)idv.toUInt(&ok);
                    if (!ok)
                    {
                        xml.raiseError(QLatin1String("Invalid device ID"));
                        continue;
                    }

                    idSet.insert(deviceId);
                    DVRCamera camera = DVRCamera::getCamera(this, deviceId);
                    camera.setOnline(true);
                    if (!camera.parseXML(xml))
                    {
                        if (!xml.hasError())
                            xml.raiseError(QLatin1String("Device parsing failed"));
                        continue;
                    }

                    if (!m_cameras.contains(camera))
                    {
                        m_cameras.append(camera);
                        emit cameraAdded(camera);
                    }
                }
            }
            break;
        }
        else
            xml.skipCurrentElement();
    }

    if (!hasDevicesElement)
        xml.raiseError(QLatin1String("Invalid format: no devices element"));

    if (xml.hasError())
    {
        qWarning() << "DVRServer: Error while parsing camera list:" << xml.errorString();
        return;
    }

    for (int i = 0; i < m_cameras.size(); ++i)
    {
        if (!idSet.contains(m_cameras[i].uniqueId()))
        {
            DVRCamera c = m_cameras[i];
            m_cameras.removeAt(i);
            qDebug("DVRServer: camera %d removed", c.uniqueId());
            emit cameraRemoved(c);
            c.removed();
            --i;
        }
    }

    if (!devicesLoaded || (wasEmpty && !m_cameras.isEmpty()))
    {
        devicesLoaded = true;
        emit devicesReady();
    }
}

void DVRServer::updateStatsReply()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;

    reply->deleteLater();

    QString message;

    if (reply->error() != QNetworkReply::NoError)
    {
        message = tr("Status request error: %1").arg(reply->errorString());
    }
    else
    {
        QByteArray data = reply->readAll();
        QXmlStreamReader xml(data);

        bool hadMessageElement = false;
        while (xml.readNextStartElement())
        {
            if (xml.name() == QLatin1String("stats"))
            {
                while (xml.readNext() != QXmlStreamReader::Invalid)
                {
                    if (xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == QLatin1String("stats"))
                        break;
                    else if (xml.tokenType() != QXmlStreamReader::StartElement)
                        continue;

                    if (xml.name() == QLatin1String("message"))
                    {
                        hadMessageElement = true;
                        QString text = xml.readElementText();
                        if (!text.isEmpty())
                            message = text;
                    }
                    else if (xml.name() == QLatin1String("bc-server-running") &&
                             xml.readElementText().trimmed() == QLatin1String("down"))
                        message = tr("Server process stopped");
                }
            }
        }

        if (!hadMessageElement)
            message = tr("Status request error: invalid server response");
    }

    if (message != m_statusAlertMessage) {
        m_statusAlertMessage = message;
        emit statusAlertMessageChanged(message);
    }
}

void DVRServer::disconnected()
{
    while (!m_cameras.isEmpty())
    {
        DVRCamera c = m_cameras.takeFirst();
        c.setOnline(false);
        emit cameraRemoved(c);
        c.removed();
    }
    devicesLoaded = false;
    m_statusAlertMessage.clear();
    emit statusAlertMessageChanged(QString());
}

bool DVRServer::isKnownCertificate(const QSslCertificate &certificate) const
{
    QByteArray knownDigest = readSetting(QLatin1String("sslDigest")).toByteArray();
    if (knownDigest.isEmpty())
    {
        /* If we don't know a certificate yet, we treat the first one we see as
         * correct. This is insecure, obviously, but it's a much nicer way to behave
         * for what we're doing here. */
        const_cast<DVRServer*>(this)->setKnownCertificate(certificate);
        return true;
    }

    return (certificate.digest(QCryptographicHash::Sha1) == knownDigest);
}

void DVRServer::setKnownCertificate(const QSslCertificate &certificate)
{
    writeSetting(QLatin1String("sslDigest"), certificate.digest(QCryptographicHash::Sha1));
}

QString DVRServer::displayName() const
{
    return m_displayName;
}

QString DVRServer::hostname() const
{
    return m_hostname;
}

int DVRServer::serverPort() const
{
    return m_port;
}

int DVRServer::rtspPort() const
{
    return serverPort() + 1;
}

QString DVRServer::username() const
{
    return m_username;
}

QString DVRServer::password() const
{
    return m_password;
}

bool DVRServer::autoConnect() const
{
    return m_autoConnect;
}

void DVRServer::readFromSettings()
{
    m_displayName = readSetting(QLatin1String("displayName")).toString();
    m_hostname = readSetting(QLatin1String("hostname")).toString();
    m_port = readSetting(QLatin1String("port")).toInt();
    if (!m_port)
        m_port = 7001;
    m_username = readSetting(QLatin1String("username")).toString();
    m_password = readSetting(QLatin1String("password")).toString();
    m_autoConnect = readSetting(QLatin1String("autoConnect"), true).toBool();
}
