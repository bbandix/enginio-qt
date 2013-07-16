#include "enginiobackendconnection_p.h"
#include "enginioclient.h"
#include "enginioclient_p.h"
#include "enginioreply.h"

#include <QtCore/qbytearray.h>
#include <QtCore/qcryptographichash.h>
#include <QtCore/QtEndian>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsonvalue.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qstring.h>
#include <QtCore/quuid.h>
#include <QtNetwork/qtcpsocket.h>

#define CRLF "\r\n"
#define FIN 0x80
#define OPC 0x0F
#define LEN 0x7F

namespace {
const qint64 DefaultHeaderLength = 2;

const QString HttpResponseStatus(QStringLiteral("HTTP/1\\.1\\s([0-9]{3})\\s"));
const QString SecWebSocketAcceptHeader(QStringLiteral("Sec-WebSocket-Accept:\\s(.{28})" CRLF));
const QString UpgradeHeader(QStringLiteral("Upgrade:\\s(.+)" CRLF));
const QString ConnectionHeader(QStringLiteral("Connection:\\s(.+)" CRLF));

QString gBase64EncodedSha1VerificationKey;

void computeBase64EncodedSha1VerificationKey(const QString &base64Key)
{
    // http://tools.ietf.org/html/rfc6455#section-4.2.2 §5./ 4.
    QString webSocketMagicString(QStringLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    webSocketMagicString.prepend(base64Key);
    gBase64EncodedSha1VerificationKey = QString::fromUtf8(QCryptographicHash::hash(webSocketMagicString.toUtf8(), QCryptographicHash::Sha1).toBase64());
}

const QString generateBase64EncodedUniqueKey()
{
    QByteArray nonce = QUuid::createUuid().toByteArray();

    // Remove unneeded pretty-formatting.
    // before: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
    // after:  "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    nonce.chop(1);      // }
    nonce.remove(0, 1); // {
    nonce.remove(23, 1);
    nonce.remove(18, 1);
    nonce.remove(13, 1);
    nonce.remove(8, 1);

    QString secWebSocketKeyBase64 = QString::fromUtf8(nonce.toBase64());
    computeBase64EncodedSha1VerificationKey(secWebSocketKeyBase64);

    return secWebSocketKeyBase64;
}

int extractResponseStatus(QString responseString)
{
    static const QRegularExpression re(HttpResponseStatus);
    QRegularExpressionMatch match = re.match(responseString);
    return match.captured(1).toInt();
}

const QString extractResponseHeader(QString pattern, QString responseString, bool ignoreCase = true)
{
    const QRegularExpression re(pattern);
    QRegularExpressionMatch match = re.match(responseString);

    if (ignoreCase)
        return match.captured(1).toLower();

    return match.captured(1);
}

const QByteArray constructOpeningHandshake(const QUrl& url)
{
    static QString host = QStringLiteral("%1:%2");
    static QString resourceUri = QStringLiteral("%1?%2");
    static QString request = QStringLiteral("GET %1 HTTP/1.1" CRLF
                                            "Host: %2" CRLF
                                            "Upgrade: websocket" CRLF
                                            "Connection: upgrade" CRLF
                                            "Sec-WebSocket-Key: %3" CRLF
                                            "Sec-WebSocket-Version: 13" CRLF
                                            CRLF
                                            );

    // http://tools.ietf.org/html/rfc6455#section-4.1 §2./ 7.
    // The request must include a header field with the name
    // Sec-WebSocket-Key. The value of this header field must be a
    // nonce consisting of a randomly selected 16-byte value that has
    // been base64-encoded.
    // The nonce must be selected randomly for each connection.

    return request.arg(resourceUri.arg(url.path(QUrl::FullyEncoded), url.query(QUrl::FullyEncoded))
                       , host.arg(url.host(QUrl::FullyEncoded), QString::number(url.port(8080)))
                       , generateBase64EncodedUniqueKey()
                       ).toUtf8();
}
} // namespace

/*!
    \brief Class to establish a stateful connection to a backend.
    The communication is based on the WebSocket protocol.
    \sa connectToBackend

    \internal
*/

EnginioBackendConnection::EnginioBackendConnection(QObject *parent)
    : QObject(parent)
    , _protocolOpcode(ContinuationFrameOp)
    , _protocolDecodeState(HandshakePending)
    , _isFinalFragment(false)
    , _isPayloadMasked(false)
    , _payloadLength(0)
    , _tcpSocket(new QTcpSocket(this))
    , _client(new EnginioClient(this))
{
    _tcpSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    _tcpSocket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    QObject::connect(_client, SIGNAL(error(EnginioReply*)), this, SLOT(onEnginioError(EnginioReply*)));
    QObject::connect(_client, SIGNAL(finished(EnginioReply*)), this, SLOT(onEnginioFinished(EnginioReply*)));

    QObject::connect(_tcpSocket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onSocketConnectionError(QAbstractSocket::SocketError)));
    QObject::connect(_tcpSocket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SLOT(onSocketStateChanged(QAbstractSocket::SocketState)));
    QObject::connect(_tcpSocket, SIGNAL(readyRead()), this, SLOT(onSocketReadyRead()));
}

void EnginioBackendConnection::onEnginioError(EnginioReply *reply)
{
    Q_ASSERT(reply);
    qDebug() << "\n\n### EnginioBackendConnection ERROR";
    qDebug() << reply->errorString();
    reply->dumpDebugInfo();
    qDebug() << "\n###\n";
}

void EnginioBackendConnection::onEnginioFinished(EnginioReply *reply)
{
    QJsonValue urlValue = reply->data()["expiringUrl"];

    if (!urlValue.isString()) {
        qDebug() << "## Retrieving connection url failed.";
        return;
    }

    qDebug() << "## Initiating WebSocket connection.";

    _socketUrl = QUrl(urlValue.toString());
    _tcpSocket->connectToHost(_socketUrl.host(), _socketUrl.port(8080));

    reply->deleteLater();
}

void EnginioBackendConnection::protocolError(const char* message)
{
    qWarning() << QLatin1Literal(message) << QStringLiteral("Closing socket.");
    _protocolDecodeState = HandshakePending;
    _applicationData.clear();
    _payloadLength = 0;

    _tcpSocket->close();
}

void EnginioBackendConnection::onSocketConnectionError(QAbstractSocket::SocketError error)
{
    protocolError("Socket connection error.");
    qWarning() << "\t\t->" << error;
}

void EnginioBackendConnection::onSocketStateChanged(QAbstractSocket::SocketState socketState)
{
    switch (socketState) {
    case QAbstractSocket::ConnectedState:
        qDebug() << "## Initiating WebSocket handshake to:\n"
                 << _socketUrl;
        _protocolDecodeState = HandshakePending;
        // The protocol handshake will appear to the HTTP server
        // to be a regular GET request with an Upgrade offer.
        _tcpSocket->write(constructOpeningHandshake(_socketUrl));
        break;
    case QAbstractSocket::UnconnectedState:
        _protocolDecodeState = HandshakePending;
        emit stateChanged(DisconnectedState);
        break;
    default:
        break;
    }
}

void EnginioBackendConnection::onSocketReadyRead()
{
    //     WebSocket Protocol (RFC6455)
    //     Base Framing Protocol
    //     http://tools.ietf.org/html/rfc6455#section-5.2
    //
    //      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    //     +-+-+-+-+-------+-+-------------+-------------------------------+
    //     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
    //     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
    //     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
    //     | |1|2|3|       |K|             |                               |
    //     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
    //     |     Extended payload length continued, if payload len == 127  |
    //     + - - - - - - - - - - - - - - - +-------------------------------+
    //     |                               |Masking-key, if MASK set to 1  |
    //     +-------------------------------+-------------------------------+
    //     | Masking-key (continued)       |          Payload Data         |
    //     +-------------------------------- - - - - - - - - - - - - - - - +
    //     :                     Payload Data continued ...                :
    //     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
    //     |                     Payload Data continued ...                |
    //     +---------------------------------------------------------------+

    while (_tcpSocket->bytesAvailable()) {
        switch (_protocolDecodeState) {
        case HandshakePending: {
            QString response = QString::fromUtf8(_tcpSocket->readAll());

            int statusCode = extractResponseStatus(response);
            QString secWebSocketAccept = extractResponseHeader(SecWebSocketAcceptHeader, response, /* ignoreCase */ false);
            bool hasValidKey = secWebSocketAccept == gBase64EncodedSha1VerificationKey;

            if (statusCode != 101 || !hasValidKey
                    || extractResponseHeader(UpgradeHeader, response) != QStringLiteral("websocket")
                    || extractResponseHeader(ConnectionHeader, response) != QStringLiteral("upgrade")
                    )
                return protocolError("Handshake failed!");

            _protocolDecodeState = FrameHeaderPending;
            emit stateChanged(ConnectedState);
        } // Fall-through.

        case FrameHeaderPending: {
            if (_tcpSocket->bytesAvailable() < DefaultHeaderLength)
                return;

            char data[DefaultHeaderLength];
            if (_tcpSocket->read(data, DefaultHeaderLength) != DefaultHeaderLength)
                return protocolError("Reading header failed!");

            if (!_payloadLength) {
                // This is the initial frame header data.
                _isFinalFragment = (data[0] & FIN);
                _protocolOpcode = static_cast<WebSocketOpcode>(data[0] & OPC);
                _isPayloadMasked = (data[1] & FIN);
                _payloadLength = (data[1] & LEN);


                // TODO:
                // - Implement closing handshake
                // - Handle client-to-server masking (possibly needed when data comes from
                //   another client e.g. client->server->client)
                // - Handle large payload if we need it.
                if (_isPayloadMasked)
                    return protocolError("Masked payload not supported.");

                // Large payload.
                if (_payloadLength == 127)
                    return protocolError("Large payload not supported.");

                // For data length 0-125 LEN is the payload length.
                if (_payloadLength < 126)
                    _protocolDecodeState = PayloadDataPending;

            } else {
                Q_ASSERT(_payloadLength == 126);
                // Normal sized payload: 2 bytes interpreted as the payload
                // length expressed in network byte order (e.g. big endian).
                _payloadLength = qFromBigEndian<quint16>(reinterpret_cast<uchar*>(data));
                _protocolDecodeState = PayloadDataPending;
            }

            break;
        }

        case PayloadDataPending: {
            if (static_cast<quint64>(_tcpSocket->bytesAvailable()) < _payloadLength)
                return;

            // TODO: Unmask the data here if _isPayloadMasked is true.
            _applicationData.append(_tcpSocket->read(_payloadLength));
            _protocolDecodeState = FrameHeaderPending;
            _payloadLength = 0;

            if (!_isFinalFragment)
                break;

            if (_protocolOpcode == TextFrameOp)
                emit dataReceived(QJsonDocument::fromJson(_applicationData).object());
            else {
                protocolError("WebSocketOpcode not yet supported");
                qWarning() << "\t\t->" << _protocolOpcode;
            }

            _applicationData.clear();

            break;
        }
        }
    }
}

void EnginioBackendConnection::setServiceUrl(const QUrl &serviceUrl)
{
    _client->setServiceUrl(serviceUrl);
}

/*!
    \brief Establish a stateful connection to the backend specified by
    \a backendId and \a backendSecret.
    Optionally, to let the server only send specific messages of interest,
    a \a messageFilter can be provided with the following json scheme:

    {
        "data": {
            objectType: 'objects.todos'
        },
        "event": "create"
    }

    The "event" property can be one of "create", "update" or "delete".

    \internal
*/

void EnginioBackendConnection::connectToBackend(const QByteArray &backendId
                                                , const QByteArray &backendSecret
                                                , const QJsonObject &messageFilter)
{
    qDebug() << "## Requesting WebSocket url.";
    _client->setBackendId(backendId);
    _client->setBackendSecret(backendSecret);
    QUrl url(_client->serviceUrl());
    url.setPath(QStringLiteral("/v1/stream_url"));

    QByteArray filter = QJsonDocument(messageFilter).toJson(QJsonDocument::Compact);
    filter.prepend("filter=");
    url.setQuery(QString::fromUtf8(filter));

    QJsonObject headers;
    headers["Accept"] = QStringLiteral("application/json");
    QJsonObject data;
    data["headers"] = headers;

    emit stateChanged(ConnectingState);
    _client->customRequest(url, QByteArrayLiteral("GET"), data);
}
