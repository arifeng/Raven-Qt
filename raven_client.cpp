#include "raven_client.h"

#include <time.h>

#include <QApplication>
#include <QDebug>
#include <QTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUuid>

namespace {

int g_next_request_id;

char kRequestIdProperty[] = "RequestIdProperty";

}

const char Raven::kClientName[] = "Raven-Qt";
const char Raven::kClientVersion[] = "0.2.1";

Raven* g_client;

Raven::Raven()
  : initialized_(false) {
    g_client = this;
}

Raven::~Raven() {
    waitForIdle();
    Q_ASSERT(pending_request_.isEmpty());
    g_client = NULL;
}

// static
Raven* Raven::instance() {
    if (!g_client)
        g_client = new Raven();

    return g_client;
}

bool Raven::initialize(const QString& DSN) {
    waitLoop_ = 0;
    // If an empty DSN is passed, you should treat it as valid option 
    // which signifies disabling the client.
    if (DSN.isEmpty()) {
        qDebug() << "Raven client is disabled.";
        return true;
    }

    QUrl url(DSN);
    if (!url.isValid())
        return false;

    protocol_ = url.scheme();
    public_key_ = url.userName();
    secret_key_ = url.password();
    host_ = url.host();
    path_ = url.path();
    project_id_ = url.fileName();

    int i = path_.lastIndexOf('/');
    if (i >= 0)
        path_ = path_.left(i);

    int port = url.port(80);
    if (port != 80)
        host_.append(":").append(QString::number(port));

    qDebug() << "Raven client is ready.";

    initialized_ = true;
    return true;
}

void Raven::waitForIdle()
{
    if ( waitLoop_ )
    {
        qCritical() << "Recursive call Raven::waitForIdle"; 
        return;
    }

    if (pending_request_.size()) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot( true );
        QObject::connect( &timer, SIGNAL( timeout() ), &loop, SLOT( quit() ) );
        waitLoop_ = &loop;
        const int timeout = 2000;
        timer.start( timeout );
        loop.exec( QEventLoop::ExcludeUserInputEvents );
        if ( timer.isActive() ) qDebug() << "Raven client finished, took " << timeout - timer.remainingTime() << " ms";
        else qDebug() << "Raven loop ended on timeout.";
        waitLoop_ = 0;
    }
}

void Raven::set_global_tags(const QString& key, const QString& value) {
    if (!key.isEmpty() && !value.isEmpty())
        global_tags_[key] = value;
}

void Raven::set_user_data(const QString& key, const QString& value) {
    if (!key.isEmpty() && !value.isEmpty())
        user_data_[key] = value;
}

// static
// According to http://sentry.readthedocs.org/en/latest/developer/client/
void Raven::captureMessage(
    Level level,
    const QString& from_here,
    const QString& message,
    QJsonObject extra,
    QJsonObject tags) {
    Raven* instance = Raven::instance();
    if (!instance->initialized_) {
        qDebug() << "Sentry has not been initialized succefully. Failed to send message.";
        return;
    }

    QJsonObject jdata;
    jdata["event_id"] = QString(QUuid::createUuid().toRfc4122().toHex());
    jdata["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    jdata["server_name"] = QHostInfo::localHostName();
    jdata["user"] = instance->GetUserInfo();
    jdata["level"] = GetLevelString(level);
    jdata["culprit"] = from_here;
    jdata["message"] = message;
    jdata["logger"] = kClientName;
    jdata["platform"] = "C++/Qt";

    // Add global tags
    const QJsonObject& gtags = instance->global_tags_;
    if (!gtags.isEmpty()) {
        for (auto it = gtags.begin(); it != gtags.end(); ++it)
            tags[it.key()] = *it;
    }

    if (!tags.isEmpty())
        jdata["tags"] = tags;
    if (!extra.isEmpty())
        jdata["extra"] = extra;

    //jdata["modules"] = QJsonObject();

    instance->Send(jdata);
}

void Raven::Send(const QJsonObject& jbody) {
    QString client_info = QString("%1/%2").
        arg(kClientName).arg(kClientVersion);

    QString auth_info = QString("Sentry sentry_version=5,sentry_client=%1,sentry_timestamp=%2,sentry_key=%3,sentry_secret=%4").
        arg(client_info, QString::number(time(NULL)), public_key_, secret_key_);

    // http://192.168.1.x:9000/api/4/store/
    QString url = QString("%1://%2%3/api/%4/store/").
        arg(protocol_).arg(host_).arg(path_).arg(project_id_);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, client_info);
    request.setRawHeader("X-Sentry-Auth", auth_info.toUtf8());

    QByteArray body = QJsonDocument(jbody).toJson(QJsonDocument::Indented);

    QNetworkReply* reply = network_access_manager_.post(request, body);
    reply->setProperty(kRequestIdProperty, QVariant(++g_next_request_id));
    pending_request_[g_next_request_id] = body;
    ConnectReply(reply);
}

void Raven::ConnectReply(QNetworkReply* reply) {
    connect(reply, SIGNAL(finished()), this, SLOT(slotFinished()));
    connect(reply, SIGNAL(sslErrors(const QList<QSslError>&)), this, SLOT(slotSslError(const QList<QSslError>&)));
}

void Raven::slotFinished() {
    QNetworkReply* reply = static_cast<QNetworkReply*>(sender());

    int id = GetRequestId(reply);
    if (pending_request_.find(id) == pending_request_.end()) {
        reply->deleteLater();
        if ( pending_request_.isEmpty() && waitLoop_ ) waitLoop_->exit();
        return;
    }

    // Deal with redirection
    QUrl possibleRedirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (!possibleRedirectUrl.isEmpty() && possibleRedirectUrl != reply->url()) {
        QNetworkRequest request = reply->request();
        QByteArray body = pending_request_[id];
        request.setUrl(possibleRedirectUrl);
        QNetworkReply* new_reply = network_access_manager_.post(request, body);
        new_reply->setProperty(kRequestIdProperty, QVariant(id));
        ConnectReply(new_reply);
    } else {
        QNetworkReply::NetworkError e = reply->error();
        QByteArray res = reply->readAll();
        if (e == QNetworkReply::NoError) {
            qDebug() << "Sentry log ID " << res;
        }
        else {
            qDebug() << "Failed to send log to sentry: [" << e << "]:" << res;
        }
        
        pending_request_.remove(id);
    }

    if (pending_request_.isEmpty() && waitLoop_ ) 
        waitLoop_->exit();

    reply->deleteLater();
}

void Raven::slotSslError(const QList<QSslError>& ) {
    QNetworkReply* reply = static_cast<QNetworkReply*>(sender());
    reply->ignoreSslErrors();
}

int Raven::GetRequestId(QNetworkReply* reply) {
    QVariant var = reply->property(kRequestIdProperty);
    if (var.type() != QVariant::Int)
        return kInvalidRequestId;

    return var.toInt();
}

QJsonObject Raven::GetUserInfo() {
    QJsonObject user;
    if (!user_id_.isEmpty())
        user["id"] = user_id_;
    if (!user_name_.isEmpty())
        user["username"] = user_name_;
    if (!user_email_.isEmpty())
        user["email"] = user_email_;
    
    if (!user_data_.isEmpty()) {
        for (auto it = user_data_.begin(); it != user_data_.end(); ++it)
            user[it.key()] = *it;
    }

    user["ip_address"] = LocalAddress();
    return user;
}

QString Raven::LocalAddress() {
    QList<QHostAddress> list = QNetworkInterface::allAddresses();
    for (int nIter = 0; nIter < list.count(); nIter++) {
        if (!list[nIter].isLoopback()) {
            if (list[nIter].protocol() == QAbstractSocket::IPv4Protocol)
                return list[nIter].toString();
        }
    }

    return "0.0.0.0";
}

// static
QString Raven::locationInfo(const char* file, const char* func, int line) {
    return QString("%1 in %2 at %3").arg(file).arg(func).arg(QString::number(line));
}

// static
Raven::Level Raven::fromUnixLogLevel(int i) {
    Level level;
    switch (i) {
    case 7:  //Debug
        level = Raven::kDebug;
        break;
    case 6:  //Info
    case 5:  //Notice
        level = Raven::kInfo;
        break;
    case 4:  //Warning
        level = Raven::kWarning;
        break;
    case 3:  //Error
    case 2:  //Critical
    case 1:  //Alert
        level = Raven::kError;
        break;
    case 0:  //Emergency
        level = Raven::kFatal;
        break;
    default:
        level = Raven::kDebug;
        break;
    }

    return level;
}

// static
QString Raven::GetLevelString(Level level) {
    QString s;
    switch (level) {
    case kDebug:
        s = "debug";
        break;
    case kInfo:
        s = "info";
        break;
    case kWarning:
        s = "warning";
        break;
    case kError:
        s = "error";
        break;
    case kFatal:
        s = "fatal";
        break;
    default:
        qDebug() << "Wrong sentry log level, default to error.";
        s = "error";
        break;
    }

    return s;
}
