/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2014  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 * Copyright (C) 2006  Ishan Arora <ishan@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "server.h"

#include <algorithm>

#include <QMutableListIterator>
#include <QNetworkProxy>
#include <QSslCipher>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>

#include "base/utils/net.h"
#include "connection.h"

namespace
{
    const int KEEP_ALIVE_DURATION = 7 * 1000;  // milliseconds
    const int CONNECTIONS_LIMIT = 500;
    const int CONNECTIONS_SCAN_INTERVAL = 2;  // seconds

    QList<QSslCipher> safeCipherList()
    {
        const QStringList badCiphers = {"idea", "rc4"};
        const QList<QSslCipher> allCiphers = QSslSocket::supportedCiphers();
        QList<QSslCipher> safeCiphers;
        for (const QSslCipher &cipher : allCiphers) {
            bool isSafe = true;
            for (const QString &badCipher : badCiphers) {
                if (cipher.name().contains(badCipher, Qt::CaseInsensitive)) {
                    isSafe = false;
                    break;
                }
            }

            if (isSafe)
                safeCiphers += cipher;
        }

        return safeCiphers;
    }
}

using namespace Http;

Server::Server(IRequestHandler *requestHandler, QObject *parent)
    : QTcpServer(parent)
    , m_requestHandler(requestHandler)
    , m_https(false)
{
    setProxy(QNetworkProxy::NoProxy);
    QSslSocket::setDefaultCiphers(safeCipherList());

    QTimer *dropConnectionTimer = new QTimer(this);
    connect(dropConnectionTimer, &QTimer::timeout, this, &Server::dropTimedOutConnection);
    dropConnectionTimer->start(CONNECTIONS_SCAN_INTERVAL * 1000);
}

void Server::incomingConnection(qintptr socketDescriptor)
{
    if (m_connections.size() >= CONNECTIONS_LIMIT) return;

    QTcpSocket *serverSocket;
    if (m_https)
        serverSocket = new QSslSocket(this);
    else
        serverSocket = new QTcpSocket(this);

    if (!serverSocket->setSocketDescriptor(socketDescriptor)) {
        delete serverSocket;
        return;
    }

    if (m_https) {
        static_cast<QSslSocket *>(serverSocket)->setProtocol(QSsl::SecureProtocols);
        static_cast<QSslSocket *>(serverSocket)->setPrivateKey(m_key);
        static_cast<QSslSocket *>(serverSocket)->setLocalCertificateChain(m_certificates);
        static_cast<QSslSocket *>(serverSocket)->setPeerVerifyMode(QSslSocket::VerifyNone);
        static_cast<QSslSocket *>(serverSocket)->startServerEncryption();
    }

    Connection *c = new Connection(serverSocket, m_requestHandler, this);
    m_connections.append(c);
}

void Server::dropTimedOutConnection()
{
    QMutableListIterator<Connection *> i(m_connections);
    while (i.hasNext()) {
        auto connection = i.next();
        if (connection->isClosed() || connection->hasExpired(KEEP_ALIVE_DURATION)) {
            delete connection;
            i.remove();
        }
    }
}

bool Server::setupHttps(const QByteArray &certificates, const QByteArray &privateKey)
{
    const QList<QSslCertificate> certs {Utils::Net::loadSSLCertificate(certificates)};
    const QSslKey key {Utils::Net::loadSSLKey(privateKey)};

    if (certs.isEmpty() || key.isNull()) {
        disableHttps();
        return false;
    }

    m_key = key;
    m_certificates = certs;
    m_https = true;
    return true;
}

void Server::disableHttps()
{
    m_https = false;
    m_certificates.clear();
    m_key.clear();
}
