/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://qt.digia.com/contact-us
**
** This file is part of the Enginio Qt Client Library.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia. For licensing terms and
** conditions see http://qt.digia.com/licensing. For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights. These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file. Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
****************************************************************************/

#include "enginiomodel.h"
#include "enginioreply.h"
#include "enginioclient_p.h"
#include "enginiofakereply_p.h"
#include "enginiodummyreply_p.h"

#include <QtCore/qobject.h>
#include <QtCore/qvector.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qdatetime.h>
#include <QtCore/quuid.h>

struct EnginioModelPrivateAttachedData
{
    uint ref;
    int row;
    EnginioReply *createReply;
};
Q_DECLARE_TYPEINFO(EnginioModelPrivateAttachedData, Q_PRIMITIVE_TYPE);

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug dbg, const EnginioModelPrivateAttachedData &a)
{
    dbg.nospace() << "EnginioModelPrivateAttachedData(";
    dbg.nospace() << a.ref << ", "<< a.row;
    dbg.nospace() << ')';
    return dbg.space();
}
#endif

class AttachedDataContainer: public QHash<QString /* object id */, EnginioModelPrivateAttachedData>
{
    // TODO QHash is not the right structure, we need to index by id, row and we want to make
    // bulk update of the data.
    typedef EnginioModelPrivateAttachedData AttachedData;
    typedef QHash<QString /* object id */, AttachedData> Base;
public:
    using Base::contains;
    using Base::value;

    bool contains(int row) const
    {
        // TODO optimize it
        for (Base::const_iterator i = constBegin();
             i != constEnd();
             ++i) {
            if (i.value().row == row)
                return true;
        }
        return false;
    }

    AttachedData value(int row) const
    {
        // TODO optimize it
        for (Base::const_iterator i = constBegin();
             i != constEnd();
             ++i) {
            if (i.value().row == row)
                return i.value();
        }
        Q_UNREACHABLE();
    }

    void updateAllDataAfterRow(const int row) {
        // TODO optimize it is almost O(n log(n))
        QList<QString> keys = this->keys();
        foreach (const QString &key, keys) {
            AttachedData &data = (*this)[key];
            if (data.row > row)
                --data.row;
            else if (data.row == row)
                data.row = -1;
        }
    }

    AttachedData ref(const QString &id, int row)
    {
        AttachedData &data = (*this)[id];
        ++data.ref;
        Q_ASSERT(data.ref == 1 || data.row == row);
        data.row = row;
        return data;
    }

    AttachedData ref(int row)
    {
        // TODO optimize it
        Base::iterator i = begin();
        for (; i != end(); ++i) {
            if (i.value().row == row)
                break;
        }
        AttachedData &data = *i;
        ++data.ref;
        return data;
    }

    AttachedData deref(const QString &id)
    {
        Q_ASSERT(contains(id));
        AttachedData attachedData = take(id);
        if (--attachedData.ref)
            insert(id, attachedData);
        return attachedData;
    }
};

class EnginioModelPrivate {
    QJsonObject _query;
    EnginioClient *_enginio;
    EnginioClient::Operation _operation;
    EnginioModel *q;
    QVector<QMetaObject::Connection> _connections;

    const static int FullModelReset;
    const static int IncrementalModelUpdate;
    mutable QMap<const EnginioReply*, QPair<int /*row*/, QJsonObject> > _dataChanged;
    typedef EnginioModelPrivateAttachedData AttachedData;
    AttachedDataContainer _attachedData;
    int _latestRequestedOffset;
    bool _canFetchMore;

    unsigned _rolesCounter;
    QHash<int, QString> _roles;

    QJsonArray _data; // TODO replace by a sparse array, and add laziness

    class EnginioDestroyed
    {
        EnginioModelPrivate *model;
    public:
        EnginioDestroyed(EnginioModelPrivate *m)
            : model(m)
        {
            Q_ASSERT(m);
        }
        void operator ()()
        {
            model->setEnginio(0);
        }
    };

    class FinishedRequest
    {
        EnginioModelPrivate *model;
    public:
        FinishedRequest(EnginioModelPrivate *m)
            : model(m)
        {
            Q_ASSERT(m);
        }

        void operator ()(const EnginioReply *response)
        {
            model->finishedRequest(response);
        }
    };

    class QueryChanged
    {
        EnginioModelPrivate *model;
    public:
        QueryChanged(EnginioModelPrivate *m)
            : model(m)
        {
            Q_ASSERT(m);
        }

        void operator ()()
        {
            model->execute();
        }

    };

public:
    EnginioModelPrivate(EnginioModel *q_ptr)
        : _enginio(0)
        , _operation()
        , q(q_ptr)
        , _latestRequestedOffset(0)
        , _canFetchMore(false)
        , _rolesCounter(EnginioModel::SyncedRole)
    {
        QObject::connect(q, &EnginioModel::queryChanged, QueryChanged(this));
        QObject::connect(q, &EnginioModel::operationChanged, QueryChanged(this));
        QObject::connect(q, &EnginioModel::enginioChanged, QueryChanged(this));
    }

    ~EnginioModelPrivate()
    {
        foreach (const QMetaObject::Connection &connection, _connections)
            QObject::disconnect(connection);
    }

    EnginioClient *enginio() const Q_REQUIRED_RESULT
    {
        return _enginio;
    }

    void setEnginio(const EnginioClient *enginio)
    {
        if (_enginio) {
            foreach(const QMetaObject::Connection &connection, _connections)
                QObject::disconnect(connection);
            _connections.clear();
        }
        _enginio = const_cast<EnginioClient*>(enginio);
        if (_enginio) {
            _connections.append(QObject::connect(_enginio, &EnginioClient::finished, FinishedRequest(this)));
            _connections.append(QObject::connect(_enginio, &QObject::destroyed, EnginioDestroyed(this)));
            _connections.append(QObject::connect(_enginio, &EnginioClient::backendIdChanged, QueryChanged(this)));
            _connections.append(QObject::connect(_enginio, &EnginioClient::backendSecretChanged, QueryChanged(this)));
        }
        emit q->enginioChanged(_enginio);
    }

    QJsonObject query() Q_REQUIRED_RESULT
    {
        return _query;
    }

    EnginioReply *append(const QJsonObject &value)
    {
        QJsonObject object(value);
        object[EnginioString::objectType] = _query[EnginioString::objectType]; // TODO think about it, it means that not all queries are valid
        EnginioReply *ereply = _enginio->create(object, _operation);
        QString temporaryId = QString::fromLatin1("tmp") + QUuid::createUuid().toString();
        object[EnginioString::id] = temporaryId;
        const int row = _data.count();
        AttachedData data = {1, row, ereply};
        if (!row) { // the first item need to update roles
            q->beginResetModel();
            _attachedData.insert(temporaryId, data);
            _data.append(value);
            syncRoles();
            _dataChanged.insert(ereply, qMakePair(row, object));
            q->endResetModel();
        } else {
            q->beginInsertRows(QModelIndex(), _data.count(), _data.count());
            _attachedData.insert(temporaryId, data);
            _data.append(value);
            _dataChanged.insert(ereply, qMakePair(row, object));
            q->endInsertRows();
        }
        return ereply;
    }

    struct SwapNetworkReplyBase
    {
        EnginioReply *_reply;
        EnginioModelPrivate *_model;
        QJsonObject _object;
        QString _tmpId;

        void markAsError(QByteArray msg)
        {
            EnginioClientPrivate *client = EnginioClientPrivate::get(_model->_enginio);
            EnginioFakeReply *nreply = new EnginioFakeReply(client, constructErrorMessage(msg));
            _reply->setNetworkReply(nreply);
        }

        int getCurrentRow() { return _model->_attachedData.deref(_tmpId).row; }

        QString getAndSetCurrentId(EnginioReply *finishedCreateReply)
        {
            QString id = finishedCreateReply->data()[EnginioString::id].toString();
            Q_ASSERT(!id.isEmpty());
            _object[EnginioString::id] = id;
            return id;
        }

        void swapNetworkReply(EnginioReply *ereply)
        {
            QPair<int /*row*/, QJsonObject> data = _model->_dataChanged.take(ereply);
            _model->_dataChanged.insert(_reply, data);
            _reply->swapNetworkReply(ereply);
            ereply->deleteLater();
        }
    };

    struct SwapNetworkReplyForRemove
    {
        SwapNetworkReplyBase d;
        void operator ()(EnginioReply *finishedCreateReply)
        {
            if (finishedCreateReply->isError()) {
                d.markAsError(QByteArrayLiteral("Dependent create query failed, so object coudl not be removed"));
            } else {
                const int row = d.getCurrentRow();
                QString id = d.getAndSetCurrentId(finishedCreateReply);
                EnginioReply *ereply = d._model->removeNow(row, d._object, id);
                d.swapNetworkReply(ereply);
            }
        }
    };

    EnginioReply *remove(int row)
    {
        QJsonObject oldObject = _data.at(row).toObject();
        QString id = oldObject[EnginioString::id].toString();
        if (id.isEmpty())
            return removeDelayed(row, oldObject);
        return removeNow(row, oldObject, id);
    }

    EnginioReply *removeDelayed(int row, const QJsonObject &oldObject)
    {
        // We are about to remove a not synced new item. The item do not have id yet,
        // so we can not make a request now, we need to wait for finished signal.
        EnginioReply *ereply, *createReply;
        QString tmpId;
        Q_ASSERT(oldObject[EnginioString::id].toString().isEmpty());
        delayedOperation(row, &ereply, &tmpId, &createReply);
        SwapNetworkReplyForRemove swapNetworkReply = {{ereply, this, oldObject, tmpId}};
        QObject::connect(createReply, &EnginioReply::finished, swapNetworkReply);
        return ereply;
    }

    EnginioReply *removeNow(int row, const QJsonObject &oldObject, const QString &id)
    {
        Q_ASSERT(!id.isEmpty());
        _attachedData.ref(id, row); // TODO if refcount is > 1 then do not emit dataChanged
        EnginioReply *ereply = _enginio->remove(oldObject, _operation);
        _dataChanged.insert(ereply, qMakePair(row, oldObject));
        QVector<int> roles(1);
        roles.append(EnginioModel::SyncedRole);
        emit q->dataChanged(q->index(row), q->index(row) , roles);
        return ereply;
    }

    EnginioReply *setValue(int row, const QString &role, const QVariant &value)
    {
        int key = _roles.key(role, EnginioModel::InvalidRole);
        return setData(row, value, key);
    }

    void setQuery(const QJsonObject &query)
    {
        _query = query;

        if (_query.contains(EnginioString::pageSize)) {
            const int pageSize = _query[EnginioString::pageSize].toDouble();
            const QString limitString(EnginioString::limit);
            const QString offsetString(EnginioString::offset);
            const unsigned limit = _query[limitString].toDouble();
            const unsigned offset = _query[offsetString].toDouble();
            if (limit)
                qWarning() << "EnginioModel::setQuery()" << "'limit' parameter can not be used together with model pagining feature, the value will be ignored";

            if (offset) {
                qWarning() << "EnginioModel::setQuery()" << "'offset' parameter can not be used together with model pagining feature, the value will be ignored";
                _query.remove(offsetString);
            }
            _query[limitString] = pageSize;
            _canFetchMore = true;
        } else {
            _canFetchMore = false;
        }
        emit q->queryChanged(query);
    }

    EnginioClient::Operation operation() const Q_REQUIRED_RESULT
    {
        return _operation;
    }

    void setOperation(const int operation)
    {
        Q_ASSERT_X(operation >= EnginioClient::ObjectOperation, "setOperation", "Invalid operation specified.");
        _operation = static_cast<EnginioClient::Operation>(operation);
        emit q->operationChanged(_operation);
    }

    void execute()
    {
        if (!_enginio || _enginio->backendId().isEmpty() || _enginio->backendSecret().isEmpty())
            return;
        if (!_query.isEmpty()) {
            const EnginioReply *ereply = _enginio->query(_query, _operation);
            if (_canFetchMore)
                _latestRequestedOffset = _query[EnginioString::limit].toDouble();
            QObject::connect(ereply, &EnginioReply::finished, ereply, &EnginioReply::deleteLater);
            _dataChanged.insert(ereply, qMakePair(FullModelReset, QJsonObject()));
        }
    }

    void finishedRequest(const EnginioReply *response)
    {
        // We get all finished requests, check if we started this one
        if (!_dataChanged.contains(response))
            return;

        // ### TODO proper error handling
        // this kind of response happens when the backend id/secret is missing
        if (!response->data()[EnginioString::message].isNull())
            qWarning() << "Enginio: " << response->data()[EnginioString::message].toString();

        QPair<int, QJsonObject> requestInfo = _dataChanged.take(response);
        int row = requestInfo.first;
        if (row == FullModelReset) {
            q->beginResetModel();
            _attachedData.clear();
            _data = response->data()[EnginioString::results].toArray();
            syncRoles();
            _canFetchMore = _canFetchMore && _data.count() && (_query[EnginioString::limit].toDouble() <= _data.count());
            q->endResetModel();
        } else if (row == IncrementalModelUpdate) {
            Q_ASSERT(_canFetchMore);
            QJsonArray data(response->data()[EnginioString::results].toArray());
            QJsonObject query(requestInfo.second);
            int offset = query[EnginioString::offset].toDouble();
            int limit = query[EnginioString::limit].toDouble();
            int dataCount = data.count();

            int startingOffset = qMax(offset, _data.count());

            q->beginInsertRows(QModelIndex(), startingOffset, startingOffset + dataCount -1);
            for (int i = 0; i < dataCount; ++i) {
                _data.append(data[i]);
            }

            _canFetchMore = limit <= dataCount;
            q->endInsertRows();
        } else {
            QJsonObject newValue(response->data());
            QJsonObject oldValue = requestInfo.second;
            QString oldId = oldValue[EnginioString::id].toString();

            AttachedData attachedData = _attachedData.deref(oldId);

            bool removeOperation = newValue.isEmpty();
            // update the row number
            row = attachedData.row;
            if (row == -1 || response->backendStatus() == 404) {
                // The object was not found on the server, which means that it was deleted already
                if (removeOperation || row == -1) {
                    // Nothing to do, updating a removed object, that is not in the cache
                    // or removing a removed object
                    return;
                }
                // Updating a removed row. Change operation type to remove, so the cache can
                // be in sync with the server again.
                // TODO add a signal here so a developer can ask an user for a conflict
                // resolution.
                removeOperation = true;
            }
            Q_ASSERT(row >= 0 && row < _data.count());

            if (response->networkError() != QNetworkReply::NoError && response->backendStatus() != 404) {
                _data.replace(row, oldValue);
                emit q->dataChanged(q->index(row), q->index(row));
                return;
            }

            if (removeOperation) {
                q->beginRemoveRows(QModelIndex(), row, row);
                _data.removeAt(row);
                // we need to updates rows in _attachedData
                _attachedData.updateAllDataAfterRow(row);
                q->endRemoveRows();
            } else {
                QJsonObject current = _data[row].toObject();
                QDateTime currentUpdateAt = QDateTime::fromString(current[EnginioString::updatedAt].toString(), Qt::ISODate);
                QDateTime newUpdateAt = QDateTime::fromString(newValue[EnginioString::updatedAt].toString(), Qt::ISODate);
                if (newUpdateAt < currentUpdateAt) {
                    // we already have a newer version
                    return;
                }
                if (_data.count() == 1) {
                    q->beginResetModel();
                    _data.replace(row, newValue);
                    syncRoles();
                    q->endResetModel();
                } else {
                    _data.replace(row, newValue);
                    emit q->dataChanged(q->index(row), q->index(row));
                }
            }
        }
    }

    struct SwapNetworkReplyForSetData
    {
        SwapNetworkReplyBase d;
        QVariant _value;
        int _role;

        void operator ()(EnginioReply *finishedCreateReply)
        {
            if (finishedCreateReply->isError()) {
                d.markAsError(QByteArrayLiteral("Dependent create query failed, so object coudl not be updated"));
            } else {
                const int row = d.getCurrentRow();
                QString id = d.getAndSetCurrentId(finishedCreateReply);
                EnginioReply *ereply = d._model->setDataNow(row, _value, _role, d._object, id);
                d.swapNetworkReply(ereply);
            }
        }
    };

    EnginioReply *setData(const int row, const QVariant &value, int role)
    {
        if (role > EnginioModel::SyncedRole) {
            QJsonObject oldObject = _data.at(row).toObject();
            QString id = oldObject[EnginioString::id].toString();
            if (id.isEmpty())
                return setDataDelyed(row, value, role, oldObject);
            return setDataNow(row, value, role, oldObject, id);
        }
        EnginioClientPrivate *client = EnginioClientPrivate::get(_enginio);
        QNetworkReply *nreply = new EnginioFakeReply(client, constructErrorMessage(QByteArrayLiteral("EnginioModel: Trying to update an object with unknown role")));
        EnginioReply *ereply = new EnginioReply(client, nreply);
        return ereply;
    }

    void delayedOperation(int row, EnginioReply **newReply, QString *tmpId, EnginioReply **createReply)
    {
        Q_ASSERT(_attachedData.contains(row));
        AttachedData data = _attachedData.ref(row);
        *createReply = data.createReply;
        Q_ASSERT(*createReply);
        Q_ASSERT(!(*createReply)->isFinished());
        *tmpId = _dataChanged.value(*createReply).second[EnginioString::id].toString();
        Q_ASSERT(tmpId->startsWith(QString::fromLatin1("tmp")));
        EnginioClientPrivate *client = EnginioClientPrivate::get(_enginio);
        EnginioDummyReply *nreply = new EnginioDummyReply(*createReply);
        *newReply = new EnginioReply(client, nreply);
    }

    EnginioReply *setDataDelyed(int row, const QVariant &value, int role, const QJsonObject &oldObject)
    {
        // We are about to update a not synced new item. The item do not have id yet,
        // so we can not make a request now, we need to wait for finished signal.
        Q_ASSERT(role > EnginioModel::SyncedRole);
        EnginioReply *ereply, *createReply;
        QString tmpId;
        Q_ASSERT(oldObject[EnginioString::id].toString().isEmpty());
        delayedOperation(row, &ereply, &tmpId, &createReply);
        SwapNetworkReplyForSetData swapNetworkReply = {{ereply, this, oldObject, tmpId}, value, role};
        QObject::connect(createReply, &EnginioReply::finished, swapNetworkReply);
        return ereply;
    }

    EnginioReply *setDataNow(const int row, const QVariant &value, int role, const QJsonObject &oldObject, const QString &id)
    {
        Q_ASSERT(!id.isEmpty());
        Q_ASSERT(role > EnginioModel::SyncedRole);
        const QString roleName(_roles.value(role));
        QJsonObject deltaObject;
        QJsonObject newObject = oldObject;
        deltaObject[roleName] = newObject[roleName] = QJsonValue::fromVariant(value);
        deltaObject[EnginioString::id] = id;
        deltaObject[EnginioString::objectType] = newObject[EnginioString::objectType];
        EnginioReply *ereply = _enginio->update(deltaObject, _operation);
        _dataChanged.insert(ereply, qMakePair(row, oldObject));
        _attachedData.ref(id, row);
        Q_ASSERT(_attachedData.contains(id) && _attachedData[id].ref > 0);
        _data.replace(row, newObject);
        emit q->dataChanged(q->index(row), q->index(row));
        return ereply;
    }

    void syncRoles()
    {
        QJsonObject firstObject(_data.first().toObject()); // TODO it expects certain data structure in all objects, add way to specify roles

        if (!_roles.count()) {
            _roles.reserve(firstObject.count());
            _roles[EnginioModel::SyncedRole] = EnginioString::_synced; // TODO Use a proper name, can we make it an attached property in qml? Does it make sense to try?
            _roles[EnginioModel::CreatedAtRole] = EnginioString::createdAt;
            _roles[EnginioModel::UpdatedAtRole] = EnginioString::updatedAt;
            _roles[EnginioModel::IdRole] = EnginioString::id;
            _roles[EnginioModel::ObjectTypeRole] = EnginioString::objectType;
            _rolesCounter = EnginioModel::LastRole;
        }

        // estimate additional dynamic roles:
        QSet<QString> definedRoles = _roles.values().toSet();
        for (QJsonObject::const_iterator i = firstObject.constBegin(); i != firstObject.constEnd(); ++i) {
            const QString key = i.key();
            if (definedRoles.contains(key)) {
                // we skip predefined keys so we can keep constant id for them
                if (Q_UNLIKELY(key == EnginioString::_synced))
                    qWarning("EnginioModel can not be used with objects having \"_synced\" property. The property will be overriden.");
            } else
                _roles[_rolesCounter++] = i.key();
        }
    }

    QHash<int, QByteArray> roleNames() const Q_REQUIRED_RESULT
    {
        // TODO this is not optimal, but happen once, do we want to do something about it?
        QHash<int, QByteArray> roles;
        roles.reserve(_roles.count());
        for (QHash<int, QString>::const_iterator i = _roles.constBegin();
             i != _roles.constEnd();
             ++i) {
            roles.insert(i.key(), i.value().toUtf8());
        }
        return roles;
    }

    int rowCount() const Q_REQUIRED_RESULT
    {
        return _data.count();
    }

    QVariant data(unsigned row, int role) Q_REQUIRED_RESULT
    {
        if (role == EnginioModel::SyncedRole) {
            return !_attachedData.contains(row);
        }

        if (role == Qt::DisplayRole)
            return _data.at(row);

        const QJsonObject object = _data.at(row).toObject();
        if (!object.isEmpty()) {
            const QString roleName = _roles.value(role);
            if (!roleName.isEmpty())
                return object[roleName];
        }

        return QVariant();
    }

    bool canFetchMore() const Q_REQUIRED_RESULT
    {
        return _canFetchMore;
    }

    void fetchMore(int row)
    {
        int currentOffset = _data.count();
        if (!_canFetchMore || currentOffset < _latestRequestedOffset)
            return; // we do not want to spam the server, lets wait for the last fetch

        QJsonObject query(_query);

        int limit = query[EnginioString::limit].toDouble();
        limit = qMax(row - currentOffset, limit); // check if default limit is not too small

        query[EnginioString::offset] = currentOffset;
        query[EnginioString::limit] = limit;

        qDebug() << Q_FUNC_INFO << query;
        _latestRequestedOffset += limit;
        EnginioReply *ereply = _enginio->query(query, _operation);
        QObject::connect(ereply, &EnginioReply::finished, ereply, &EnginioReply::deleteLater);
        _dataChanged.insert(ereply, qMakePair(IncrementalModelUpdate, query));
    }
};

const int EnginioModelPrivate::FullModelReset = -1;
const int EnginioModelPrivate::IncrementalModelUpdate = -2;


/*!
  \class EnginioModel
  \inmodule enginio-qt
  \ingroup enginio-client
  \target EnginioModelCpp
  \brief EnginioModel represents data from Enginio as a \l QAbstractListModel.

  Example of setting a query.

  The query has to result in a list.

  Inside the delegate you have the magic properties model and index.
  Exposes the data as "model".
  index

  Sorting is done server side, as soon as data is changed locally it will be invalid.

  \note that the EnginioClient does not emit the finished and error signals for the model.

  For the QML version of this class see \l {Enginio1::EnginioModel}{EnginioModel (QML)}
*/

/*!
    Constructs a new model with \a parent as QObject parent.
*/
EnginioModel::EnginioModel(QObject *parent)
    : QAbstractListModel(parent)
    , d(new EnginioModelPrivate(this))
{}

/*!
    Destroys the model.
*/
EnginioModel::~EnginioModel()
{}

/*!
  \enum EnginioModel::Roles

  EnginioModel defines roles which represent data used by every object
  stored in the Enginio backend

  \value CreatedAtRole \c When an item was created
  \value UpdatedAtRole \c When an item was updated last time
  \value IdRole \c What is the id of an item
  \value ObjectTypeRole \c What is item type
  \value SyncedRole \c Mark if an item is in sync with the backend
  \omitvalue InvalidRole
  \omitvalue LastRole

  Additionally EnginioModel supports dynamic roles which are mapped
  directly from recieved data. EnginioModel is mapping first item properties
  to role names.

  \note Some objects may not contain value for a static role, it may happen
  for example when an item is not in sync with the backend.

  \sa EnginioModel::roleNames()
*/

/*!
  \property EnginioModel::enginio
  \brief The EnginioClient used by the model.

  \sa EnginioClient
*/
EnginioClient *EnginioModel::enginio() const
{
    return d->enginio();
}

void EnginioModel::setEnginio(const EnginioClient *enginio)
{
    if (enginio == d->enginio())
        return;
    d->setEnginio(enginio);
}

/*!
  \property EnginioModel::query
  \brief The query which returns the data for the model.

  Sorting preserved until insertion/deletion

  \sa EnginioClient::query()
*/
QJsonObject EnginioModel::query()
{
    return d->query();
}

void EnginioModel::setQuery(const QJsonObject &query)
{
    if (d->query() == query)
        return;
    return d->setQuery(query);
}

/*!
  \property EnginioModel::operation
  \brief The operation type of the query
  \sa EnginioClient::Operation, query()
  \return returns the Operation
*/
EnginioClient::Operation EnginioModel::operation() const
{
    return d->operation();
}

void EnginioModel::setOperation(EnginioClient::Operation operation)
{
    if (operation == d->operation())
        return;
    d->setOperation(operation);
}

/*!
  Append \a value to this model local cache and send a create request
  to enginio backend.
  \return reply from backend
  \sa EnginioClient::create()
*/
EnginioReply *EnginioModel::append(const QJsonObject &value)
{
    if (Q_UNLIKELY(!d->enginio())) {
        qWarning("EnginioModel::append(): Enginio client is not set");
        return 0;
    }

    return d->append(value);
}

/*!
  Remove a value from \a row in this model local cache and send
  a remove request to enginio backend.
  \return reply from backend
  \sa EnginioClient::remove()
*/
EnginioReply *EnginioModel::remove(int row)
{
    if (Q_UNLIKELY(!d->enginio())) {
        qWarning("EnginioModel::remove(): Enginio client is not set");
        return 0;
    }

    if (unsigned(row) >= unsigned(d->rowCount())) {
        EnginioClientPrivate *client = EnginioClientPrivate::get(d->enginio());
        QNetworkReply *nreply = new EnginioFakeReply(client, constructErrorMessage(QByteArrayLiteral("EnginioModel::remove: row is out of range")));
        EnginioReply *ereply = new EnginioReply(client, nreply);
        return ereply;
    }

    return d->remove(row);
}

/*!
  Update a value on \a row of this model's local cache
  and send an update request to the Enginio backend.

  The \a role is the property of the object that will be updated to be the new \a value.

  \return reply from backend
  \sa EnginioClient::update()
*/
EnginioReply *EnginioModel::setProperty(int row, const QString &role, const QVariant &value)
{
    if (Q_UNLIKELY(!d->enginio())) {
        qWarning("EnginioModel::setProperty(): Enginio client is not set");
        return 0;
    }

    if (unsigned(row) >= unsigned(d->rowCount())) {  // TODO remove as soon as we have a sparse array.
        EnginioClientPrivate *client = EnginioClientPrivate::get(d->enginio());
        QNetworkReply *nreply = new EnginioFakeReply(client, constructErrorMessage(QByteArrayLiteral("EnginioModel::setProperty: row is out of range")));
        EnginioReply *ereply = new EnginioReply(client, nreply);
        return ereply;
    }

    return d->setValue(row, role, value);
}

/*!
    \overload
    \internal
*/
Qt::ItemFlags EnginioModel::flags(const QModelIndex &index) const
{
    return QAbstractListModel::flags(index) | Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

/*!
    \overload
    Use this function to access the model data at \a index.
    With the \l roleNames() function the mapping of JSON property names to data roles used as \a role is available.
    The data returned will be JSON (for example a string for simple objects, or a JSON Object).
*/
QVariant EnginioModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= d->rowCount())
        return QVariant();

    return d->data(index.row(), role);
}

/*!
    \overload
    \internal
*/
int EnginioModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return d->rowCount();
}

/*!
    \overload
    \internal
*/
bool EnginioModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (index.row() >= d->rowCount()) // TODO remove as soon as we have a sparse array.
        return false;

    return d->setData(index.row(), value, role);
}

/*!
    \overload
    Returns the mapping of the model's roles to names.
    EnginioModel will assign the properties of the objects in the \l query()
    to roles (greater than \l Qt::UserRole).
    Use this function to map the object property names to the role integers.
*/
QHash<int, QByteArray> EnginioModel::roleNames() const
{
    return d->roleNames();
}

/*!
    \overload
    \internal
*/
void EnginioModel::fetchMore(const QModelIndex &parent)
{
    d->fetchMore(parent.row());
}

/*!
    \overload
    \internal
*/
bool EnginioModel::canFetchMore(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return d->canFetchMore();
}
