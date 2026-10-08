/****************************************************************************
** Meta object code from reading C++ file 'mainwindow.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../mainwindow.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QList>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'mainwindow.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN10MainWindowE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN10MainWindowE = QtMocHelpers::stringData(
    "MainWindow",
    "onBtnRecordClicked",
    "",
    "onBtnSettingsClicked",
    "onBtnRefreshClicked",
    "onBtnAddCamClicked",
    "onBtnSnapshotClicked",
    "onBtnLoadImageClicked",
    "onChkMixMicToggled",
    "checked",
    "onSourceComboChanged",
    "index",
    "onMicComboChanged",
    "onSysComboChanged",
    "onLayerSelectionChanged",
    "onBtnLayerUpClicked",
    "onBtnLayerDownClicked",
    "onBtnLayerDelClicked",
    "onBtnAddSourceClicked",
    "updatePreview",
    "img",
    "updateOverlay",
    "id",
    "updateAudioLevel",
    "isMic",
    "level",
    "onRecordingStateChanged",
    "isRecording",
    "msg",
    "onDurationUpdate",
    "timeStr",
    "onSourceListUpdated",
    "QList<std::pair<QString,QVariant>>",
    "sources",
    "onAudioDeviceListUpdated",
    "names",
    "ids",
    "onSnapshotTaken",
    "path",
    "onError",
    "onFileSizeReady",
    "bytes",
    "onCameraAdded",
    "rect",
    "onItemGeometryChanged"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN10MainWindowE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      27,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,  176,    2, 0x08,    1 /* Private */,
       3,    0,  177,    2, 0x08,    2 /* Private */,
       4,    0,  178,    2, 0x08,    3 /* Private */,
       5,    0,  179,    2, 0x08,    4 /* Private */,
       6,    0,  180,    2, 0x08,    5 /* Private */,
       7,    0,  181,    2, 0x08,    6 /* Private */,
       8,    1,  182,    2, 0x08,    7 /* Private */,
      10,    1,  185,    2, 0x08,    9 /* Private */,
      12,    1,  188,    2, 0x08,   11 /* Private */,
      13,    1,  191,    2, 0x08,   13 /* Private */,
      14,    0,  194,    2, 0x08,   15 /* Private */,
      15,    0,  195,    2, 0x08,   16 /* Private */,
      16,    0,  196,    2, 0x08,   17 /* Private */,
      17,    0,  197,    2, 0x08,   18 /* Private */,
      18,    0,  198,    2, 0x08,   19 /* Private */,
      19,    1,  199,    2, 0x08,   20 /* Private */,
      21,    2,  202,    2, 0x08,   22 /* Private */,
      23,    2,  207,    2, 0x08,   25 /* Private */,
      26,    2,  212,    2, 0x08,   28 /* Private */,
      29,    1,  217,    2, 0x08,   31 /* Private */,
      31,    1,  220,    2, 0x08,   33 /* Private */,
      34,    3,  223,    2, 0x08,   35 /* Private */,
      37,    1,  230,    2, 0x08,   39 /* Private */,
      39,    1,  233,    2, 0x08,   41 /* Private */,
      40,    2,  236,    2, 0x08,   43 /* Private */,
      42,    2,  241,    2, 0x08,   46 /* Private */,
      44,    2,  246,    2, 0x08,   49 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,    9,
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QImage,   20,
    QMetaType::Void, QMetaType::Int, QMetaType::QImage,   22,   20,
    QMetaType::Void, QMetaType::Bool, QMetaType::Float,   24,   25,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   27,   28,
    QMetaType::Void, QMetaType::QString,   30,
    QMetaType::Void, 0x80000000 | 32,   33,
    QMetaType::Void, QMetaType::Bool, QMetaType::QStringList, QMetaType::QStringList,   24,   35,   36,
    QMetaType::Void, QMetaType::QString,   38,
    QMetaType::Void, QMetaType::QString,   28,
    QMetaType::Void, QMetaType::QString, QMetaType::LongLong,   38,   41,
    QMetaType::Void, QMetaType::Int, QMetaType::QRect,   22,   43,
    QMetaType::Void, QMetaType::Int, QMetaType::QRect,   22,   43,

       0        // eod
};

Q_CONSTINIT const QMetaObject MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<FramelessWidget::staticMetaObject>(),
    qt_meta_stringdata_ZN10MainWindowE.offsetsAndSizes,
    qt_meta_data_ZN10MainWindowE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN10MainWindowE_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<MainWindow, std::true_type>,
        // method 'onBtnRecordClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnSettingsClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnRefreshClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnAddCamClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnSnapshotClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnLoadImageClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onChkMixMicToggled'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'onSourceComboChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'onMicComboChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'onSysComboChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        // method 'onLayerSelectionChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnLayerUpClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnLayerDownClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnLayerDelClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'onBtnAddSourceClicked'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'updatePreview'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QImage, std::false_type>,
        // method 'updateOverlay'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QImage, std::false_type>,
        // method 'updateAudioLevel'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<float, std::false_type>,
        // method 'onRecordingStateChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'onDurationUpdate'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'onSourceListUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QList<QPair<QString,QVariant>>, std::false_type>,
        // method 'onAudioDeviceListUpdated'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QStringList, std::false_type>,
        QtPrivate::TypeAndForceComplete<QStringList, std::false_type>,
        // method 'onSnapshotTaken'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'onError'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'onFileSizeReady'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        QtPrivate::TypeAndForceComplete<qint64, std::false_type>,
        // method 'onCameraAdded'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QRect, std::false_type>,
        // method 'onItemGeometryChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<int, std::false_type>,
        QtPrivate::TypeAndForceComplete<QRect, std::false_type>
    >,
    nullptr
} };

void MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MainWindow *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->onBtnRecordClicked(); break;
        case 1: _t->onBtnSettingsClicked(); break;
        case 2: _t->onBtnRefreshClicked(); break;
        case 3: _t->onBtnAddCamClicked(); break;
        case 4: _t->onBtnSnapshotClicked(); break;
        case 5: _t->onBtnLoadImageClicked(); break;
        case 6: _t->onChkMixMicToggled((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 7: _t->onSourceComboChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->onMicComboChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 9: _t->onSysComboChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1]))); break;
        case 10: _t->onLayerSelectionChanged(); break;
        case 11: _t->onBtnLayerUpClicked(); break;
        case 12: _t->onBtnLayerDownClicked(); break;
        case 13: _t->onBtnLayerDelClicked(); break;
        case 14: _t->onBtnAddSourceClicked(); break;
        case 15: _t->updatePreview((*reinterpret_cast< std::add_pointer_t<QImage>>(_a[1]))); break;
        case 16: _t->updateOverlay((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QImage>>(_a[2]))); break;
        case 17: _t->updateAudioLevel((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<float>>(_a[2]))); break;
        case 18: _t->onRecordingStateChanged((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2]))); break;
        case 19: _t->onDurationUpdate((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 20: _t->onSourceListUpdated((*reinterpret_cast< std::add_pointer_t<QList<std::pair<QString,QVariant>>>>(_a[1]))); break;
        case 21: _t->onAudioDeviceListUpdated((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QStringList>>(_a[3]))); break;
        case 22: _t->onSnapshotTaken((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 23: _t->onError((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 24: _t->onFileSizeReady((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<qint64>>(_a[2]))); break;
        case 25: _t->onCameraAdded((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QRect>>(_a[2]))); break;
        case 26: _t->onItemGeometryChanged((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QRect>>(_a[2]))); break;
        default: ;
        }
    }
}

const QMetaObject *MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN10MainWindowE.stringdata0))
        return static_cast<void*>(this);
    return FramelessWidget::qt_metacast(_clname);
}

int MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = FramelessWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 27)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 27;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 27)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 27;
    }
    return _id;
}
QT_WARNING_POP
