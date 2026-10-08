/****************************************************************************
** Meta object code from reading C++ file 'EventBus.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../EventBus.h"
#include <QtCore/qmetatype.h>
#include <QtCore/QList>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'EventBus.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.8.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN8EventBusE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN8EventBusE = QtMocHelpers::stringData(
    "EventBus",
    "cmd_changeSource",
    "",
    "type",
    "size_t",
    "id",
    "cmd_addCamera",
    "QVariant",
    "deviceVariant",
    "cmd_toggleRecording",
    "start",
    "cmd_updateConfig",
    "RecorderConfig",
    "cfg",
    "cmd_refreshDevices",
    "cmd_takeSnapshot",
    "cmd_changeAudioDevice",
    "isMic",
    "deviceId",
    "cmd_toggleMicCapture",
    "active",
    "cmd_uiFrameProcessed",
    "cmd_overlayGeometryChanged",
    "rect",
    "data_previewFrame",
    "img",
    "data_overlayUpdate",
    "data_audioLevel",
    "level",
    "state_recordingChanged",
    "isRecording",
    "msg",
    "state_durationUpdated",
    "timeStr",
    "state_sourceListUpdated",
    "QList<std::pair<QString,QVariant>>",
    "sources",
    "state_audioDeviceListUpdated",
    "names",
    "ids",
    "state_snapshotTaken",
    "path",
    "state_error",
    "state_cameraAdded"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN8EventBusE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      20,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      20,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    2,  134,    2, 0x06,    1 /* Public */,
       6,    1,  139,    2, 0x06,    4 /* Public */,
       9,    1,  142,    2, 0x06,    6 /* Public */,
      11,    1,  145,    2, 0x06,    8 /* Public */,
      14,    0,  148,    2, 0x06,   10 /* Public */,
      15,    0,  149,    2, 0x06,   11 /* Public */,
      16,    2,  150,    2, 0x06,   12 /* Public */,
      19,    1,  155,    2, 0x06,   15 /* Public */,
      21,    0,  158,    2, 0x06,   17 /* Public */,
      22,    2,  159,    2, 0x06,   18 /* Public */,
      24,    1,  164,    2, 0x06,   21 /* Public */,
      26,    2,  167,    2, 0x06,   23 /* Public */,
      27,    2,  172,    2, 0x06,   26 /* Public */,
      29,    2,  177,    2, 0x06,   29 /* Public */,
      32,    1,  182,    2, 0x06,   32 /* Public */,
      34,    1,  185,    2, 0x06,   34 /* Public */,
      37,    3,  188,    2, 0x06,   36 /* Public */,
      40,    1,  195,    2, 0x06,   40 /* Public */,
      42,    1,  198,    2, 0x06,   42 /* Public */,
      43,    2,  201,    2, 0x06,   44 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int, 0x80000000 | 4,    3,    5,
    QMetaType::Void, 0x80000000 | 7,    8,
    QMetaType::Void, QMetaType::Bool,   10,
    QMetaType::Void, 0x80000000 | 12,   13,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   17,   18,
    QMetaType::Void, QMetaType::Bool,   20,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::QRect,    5,   23,
    QMetaType::Void, QMetaType::QImage,   25,
    QMetaType::Void, QMetaType::Int, QMetaType::QImage,    5,   25,
    QMetaType::Void, QMetaType::Bool, QMetaType::Float,   17,   28,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   30,   31,
    QMetaType::Void, QMetaType::QString,   33,
    QMetaType::Void, 0x80000000 | 35,   36,
    QMetaType::Void, QMetaType::Bool, QMetaType::QStringList, QMetaType::QStringList,   17,   38,   39,
    QMetaType::Void, QMetaType::QString,   41,
    QMetaType::Void, QMetaType::QString,   31,
    QMetaType::Void, QMetaType::Int, QMetaType::QRect,    5,   23,

       0        // eod
};

Q_CONSTINIT const QMetaObject EventBus::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ZN8EventBusE.offsetsAndSizes,
    qt_meta_data_ZN8EventBusE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN8EventBusE_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<EventBus, std::true_type>,
        // method 'cmd_changeSource'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<size_t, std::false_type>,
        // method 'cmd_addCamera'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QVariant, std::false_type>,
        // method 'cmd_toggleRecording'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'cmd_updateConfig'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<RecorderConfig, std::false_type>,
        // method 'cmd_refreshDevices'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'cmd_takeSnapshot'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'cmd_changeAudioDevice'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'cmd_toggleMicCapture'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'cmd_uiFrameProcessed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'cmd_overlayGeometryChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QRect, std::false_type>,
        // method 'data_previewFrame'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QImage, std::false_type>,
        // method 'data_overlayUpdate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QImage, std::false_type>,
        // method 'data_audioLevel'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<float, std::false_type>,
        // method 'state_recordingChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'state_durationUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'state_sourceListUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QList<QPair<QString,QVariant>>, std::false_type>,
        // method 'state_audioDeviceListUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QStringList, std::false_type>,
        QtPrivate::TypeAndForceComplete<QStringList, std::false_type>,
        // method 'state_snapshotTaken'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'state_error'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'state_cameraAdded'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QRect, std::false_type>
    >,
    nullptr
} };

void EventBus::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<EventBus *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->cmd_changeSource((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<size_t>>(_a[2]))); break;
        case 1: _t->cmd_addCamera((*reinterpret_cast< std::add_pointer_t<QVariant>>(_a[1]))); break;
        case 2: _t->cmd_toggleRecording((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 3: _t->cmd_updateConfig((*reinterpret_cast< std::add_pointer_t<RecorderConfig>>(_a[1]))); break;
        case 4: _t->cmd_refreshDevices(); break;
        case 5: _t->cmd_takeSnapshot(); break;
        case 6: _t->cmd_changeAudioDevice((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 7: _t->cmd_toggleMicCapture((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 8: _t->cmd_uiFrameProcessed(); break;
        case 9: _t->cmd_overlayGeometryChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QRect>>(_a[2]))); break;
        case 10: _t->data_previewFrame((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1]))); break;
        case 11: _t->data_overlayUpdate((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QImage>>(_a[2]))); break;
        case 12: _t->data_audioLevel((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<float>>(_a[2]))); break;
        case 13: _t->state_recordingChanged((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 14: _t->state_durationUpdated((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 15: _t->state_sourceListUpdated((*reinterpret_cast< std::add_pointer_t<QList<std::pair<QString,QVariant>>>>(_a[1]))); break;
        case 16: _t->state_audioDeviceListUpdated((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[3]))); break;
        case 17: _t->state_snapshotTaken((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 18: _t->state_error((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 19: _t->state_cameraAdded((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QRect>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _q_method_type = void (EventBus::*)(int , size_t );
            if (_q_method_type _q_method = &EventBus::cmd_changeSource; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QVariant );
            if (_q_method_type _q_method = &EventBus::cmd_addCamera; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool );
            if (_q_method_type _q_method = &EventBus::cmd_toggleRecording; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(RecorderConfig );
            if (_q_method_type _q_method = &EventBus::cmd_updateConfig; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)();
            if (_q_method_type _q_method = &EventBus::cmd_refreshDevices; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)();
            if (_q_method_type _q_method = &EventBus::cmd_takeSnapshot; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool , QString );
            if (_q_method_type _q_method = &EventBus::cmd_changeAudioDevice; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool );
            if (_q_method_type _q_method = &EventBus::cmd_toggleMicCapture; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)();
            if (_q_method_type _q_method = &EventBus::cmd_uiFrameProcessed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(int , QRect );
            if (_q_method_type _q_method = &EventBus::cmd_overlayGeometryChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 9;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QImage );
            if (_q_method_type _q_method = &EventBus::data_previewFrame; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 10;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(int , QImage );
            if (_q_method_type _q_method = &EventBus::data_overlayUpdate; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 11;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool , float );
            if (_q_method_type _q_method = &EventBus::data_audioLevel; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 12;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool , QString );
            if (_q_method_type _q_method = &EventBus::state_recordingChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 13;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QString );
            if (_q_method_type _q_method = &EventBus::state_durationUpdated; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 14;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QList<QPair<QString,QVariant>> );
            if (_q_method_type _q_method = &EventBus::state_sourceListUpdated; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 15;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(bool , QStringList , QStringList );
            if (_q_method_type _q_method = &EventBus::state_audioDeviceListUpdated; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 16;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QString );
            if (_q_method_type _q_method = &EventBus::state_snapshotTaken; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 17;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(QString );
            if (_q_method_type _q_method = &EventBus::state_error; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 18;
                return;
            }
        }
        {
            using _q_method_type = void (EventBus::*)(int , QRect );
            if (_q_method_type _q_method = &EventBus::state_cameraAdded; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 19;
                return;
            }
        }
    }
}

const QMetaObject *EventBus::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *EventBus::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN8EventBusE.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int EventBus::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 20)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 20;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 20)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 20;
    }
    return _id;
}

// SIGNAL 0
void EventBus::cmd_changeSource(int _t1, size_t _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void EventBus::cmd_addCamera(QVariant _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void EventBus::cmd_toggleRecording(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void EventBus::cmd_updateConfig(RecorderConfig _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void EventBus::cmd_refreshDevices()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void EventBus::cmd_takeSnapshot()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void EventBus::cmd_changeAudioDevice(bool _t1, QString _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void EventBus::cmd_toggleMicCapture(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void EventBus::cmd_uiFrameProcessed()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void EventBus::cmd_overlayGeometryChanged(int _t1, QRect _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void EventBus::data_previewFrame(QImage _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}

// SIGNAL 11
void EventBus::data_overlayUpdate(int _t1, QImage _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void EventBus::data_audioLevel(bool _t1, float _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void EventBus::state_recordingChanged(bool _t1, QString _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void EventBus::state_durationUpdated(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}

// SIGNAL 15
void EventBus::state_sourceListUpdated(QList<QPair<QString,QVariant>> _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 15, _a);
}

// SIGNAL 16
void EventBus::state_audioDeviceListUpdated(bool _t1, QStringList _t2, QStringList _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 16, _a);
}

// SIGNAL 17
void EventBus::state_snapshotTaken(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 17, _a);
}

// SIGNAL 18
void EventBus::state_error(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 18, _a);
}

// SIGNAL 19
void EventBus::state_cameraAdded(int _t1, QRect _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 19, _a);
}
QT_WARNING_POP
