/****************************************************************************
** Meta object code from reading C++ file 'SystemController.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../SystemController.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'SystemController.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN16SystemControllerE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN16SystemControllerE = QtMocHelpers::stringData(
    "SystemController",
    "onCapturerInitFinished",
    "",
    "success",
    "onChangeSource",
    "type",
    "size_t",
    "id",
    "onAddCamera",
    "QVariant",
    "deviceVariant",
    "onToggleRecording",
    "start",
    "onUpdateConfig",
    "RecorderConfig",
    "cfg",
    "onRefreshDevices",
    "onTakeSnapshot",
    "onChangeAudioDevice",
    "isMic",
    "deviceId",
    "onToggleMicCapture",
    "active",
    "onUiFrameProcessed",
    "onOverlayGeometryChanged",
    "rect",
    "onVideoFrameCaptured",
    "img",
    "timestamp",
    "onSnapshotCaptured",
    "onDurationTimer"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN16SystemControllerE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      14,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,   98,    2, 0x08,    1 /* Private */,
       4,    2,  101,    2, 0x08,    3 /* Private */,
       8,    1,  106,    2, 0x08,    6 /* Private */,
      11,    1,  109,    2, 0x08,    8 /* Private */,
      13,    1,  112,    2, 0x08,   10 /* Private */,
      16,    0,  115,    2, 0x08,   12 /* Private */,
      17,    0,  116,    2, 0x08,   13 /* Private */,
      18,    2,  117,    2, 0x08,   14 /* Private */,
      21,    1,  122,    2, 0x08,   17 /* Private */,
      23,    0,  125,    2, 0x08,   19 /* Private */,
      24,    2,  126,    2, 0x08,   20 /* Private */,
      26,    2,  131,    2, 0x08,   23 /* Private */,
      29,    1,  136,    2, 0x08,   26 /* Private */,
      30,    0,  139,    2, 0x08,   28 /* Private */,

 // slots: parameters
    QMetaType::Void, QMetaType::Bool,    3,
    QMetaType::Void, QMetaType::Int, 0x80000000 | 6,    5,    7,
    QMetaType::Void, 0x80000000 | 9,   10,
    QMetaType::Void, QMetaType::Bool,   12,
    QMetaType::Void, 0x80000000 | 14,   15,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   19,   20,
    QMetaType::Void, QMetaType::Bool,   22,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::QRect,    7,   25,
    QMetaType::Void, QMetaType::QImage, QMetaType::LongLong,   27,   28,
    QMetaType::Void, QMetaType::QImage,   27,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject SystemController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ZN16SystemControllerE.offsetsAndSizes,
    qt_meta_data_ZN16SystemControllerE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN16SystemControllerE_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<SystemController, std::true_type>,
        // method 'onCapturerInitFinished'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'onChangeSource'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<size_t, std::false_type>,
        // method 'onAddCamera'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QVariant, std::false_type>,
        // method 'onToggleRecording'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'onUpdateConfig'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<RecorderConfig, std::false_type>,
        // method 'onRefreshDevices'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onTakeSnapshot'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onChangeAudioDevice'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'onToggleMicCapture'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'onUiFrameProcessed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onOverlayGeometryChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QRect, std::false_type>,
        // method 'onVideoFrameCaptured'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QImage &, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        // method 'onSnapshotCaptured'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QImage &, std::false_type>,
        // method 'onDurationTimer'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void SystemController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SystemController *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->onCapturerInitFinished((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 1: _t->onChangeSource((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<size_t>>(_a[2]))); break;
        case 2: _t->onAddCamera((*reinterpret_cast< std::add_pointer_t<QVariant>>(_a[1]))); break;
        case 3: _t->onToggleRecording((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 4: _t->onUpdateConfig((*reinterpret_cast< std::add_pointer_t<RecorderConfig>>(_a[1]))); break;
        case 5: _t->onRefreshDevices(); break;
        case 6: _t->onTakeSnapshot(); break;
        case 7: _t->onChangeAudioDevice((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 8: _t->onToggleMicCapture((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 9: _t->onUiFrameProcessed(); break;
        case 10: _t->onOverlayGeometryChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QRect>>(_a[2]))); break;
        case 11: _t->onVideoFrameCaptured((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 12: _t->onSnapshotCaptured((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1]))); break;
        case 13: _t->onDurationTimer(); break;
        default: ;
        }
    }
}

const QMetaObject *SystemController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SystemController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN16SystemControllerE.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SystemController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 14;
    }
    return _id;
}
QT_WARNING_POP
