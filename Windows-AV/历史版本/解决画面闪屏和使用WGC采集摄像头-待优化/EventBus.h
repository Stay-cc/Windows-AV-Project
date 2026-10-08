#ifndef EVENTBUS_H
#define EVENTBUS_H

#include <QObject>
#include <QImage>
#include <QRect>
#include <QVariant>
#include "RecorderConfig.h"

class EventBus : public QObject
{
    Q_OBJECT
public:
    static EventBus* instance() {
        static EventBus ins;
        return &ins;
    }

    // --- Commands (View -> Controller) ---
    void sendCommandChangeSource(int type, size_t id) { emit cmd_changeSource(type, id); }
    void sendCommandAddCamera(const QVariant& deviceVariant) { emit cmd_addCamera(deviceVariant); }
    void sendCommandToggleRecording(bool start) { emit cmd_toggleRecording(start); }
    void sendCommandUpdateConfig(const RecorderConfig& cfg) { emit cmd_updateConfig(cfg); }
    void sendCommandRefreshDevices() { emit cmd_refreshDevices(); }
    void sendCommandSnapshot() { emit cmd_takeSnapshot(); }
    void sendCommandChangeAudioDevice(bool isMic, const QString& deviceId) { emit cmd_changeAudioDevice(isMic, deviceId); }
    // [新增] 控制麦克风混音开关
    void sendCommandToggleMicCapture(bool active) { emit cmd_toggleMicCapture(active); }

    void sendUiFrameProcessed() { emit cmd_uiFrameProcessed(); }
    void sendOverlayGeometryChanged(int id, const QRect& rect) { emit cmd_overlayGeometryChanged(id, rect); }

    // --- Data (Controller -> View) ---
    void firePreviewFrame(const QImage& img) { emit data_previewFrame(img); }
    void fireOverlayUpdate(int id, const QImage& img) { emit data_overlayUpdate(id, img); }
    void fireAudioLevel(bool isMic, float level) { emit data_audioLevel(isMic, level); }

    // --- State (Controller -> View) ---
    void fireRecordingStateChanged(bool isRecording, const QString& msg) { emit state_recordingChanged(isRecording, msg); }
    void fireDurationUpdate(const QString& timeStr) { emit state_durationUpdated(timeStr); }
    void fireSourceListUpdated(const QList<QPair<QString, QVariant>>& sources) { emit state_sourceListUpdated(sources); }
    void fireAudioDeviceListUpdated(bool isMic, const QStringList& names, const QStringList& ids) { emit state_audioDeviceListUpdated(isMic, names, ids); }
    void fireSnapshotTaken(const QString& path) { emit state_snapshotTaken(path); }
    void fireError(const QString& msg) { emit state_error(msg); }

signals:
    // Commands
    void cmd_changeSource(int type, size_t id);
    void cmd_addCamera(QVariant deviceVariant);
    void cmd_toggleRecording(bool start);
    void cmd_updateConfig(RecorderConfig cfg);
    void cmd_refreshDevices();
    void cmd_takeSnapshot();
    void cmd_changeAudioDevice(bool isMic, QString deviceId);
    void cmd_toggleMicCapture(bool active); // [新增]
    void cmd_uiFrameProcessed();
    void cmd_overlayGeometryChanged(int id, QRect rect);

    // Data
    void data_previewFrame(QImage img);
    void data_overlayUpdate(int id, QImage img);
    void data_audioLevel(bool isMic, float level);

    // State
    void state_recordingChanged(bool isRecording, QString msg);
    void state_durationUpdated(QString timeStr);
    void state_sourceListUpdated(QList<QPair<QString, QVariant>> sources);
    void state_audioDeviceListUpdated(bool isMic, QStringList names, QStringList ids);
    void state_snapshotTaken(QString path);
    void state_error(QString msg);

private:
    EventBus() {}
    ~EventBus() {}
    Q_DISABLE_COPY(EventBus)
};

#endif // EVENTBUS_H
