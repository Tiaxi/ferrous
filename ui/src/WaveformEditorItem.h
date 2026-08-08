// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>

#include <chrono>
#include <vector>

class QHoverEvent;
class QMouseEvent;
class QPainter;
class QQuickWindow;
class QWheelEvent;

class WaveformEditorItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QByteArray overviewData READ overviewData WRITE setOverviewData NOTIFY overviewDataChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(bool showFpsOverlay READ showFpsOverlay WRITE setShowFpsOverlay NOTIFY showFpsOverlayChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(qulonglong mutedChannelsMask READ mutedChannelsMask WRITE setMutedChannelsMask NOTIFY mutedChannelsMaskChanged)
    Q_PROPERTY(int soloedChannel READ soloedChannel WRITE setSoloedChannel NOTIFY soloedChannelChanged)
    Q_PROPERTY(int channelCountHint READ channelCountHint WRITE setChannelCountHint NOTIFY channelCountHintChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY channelCountChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(bool samplePointsVisible READ samplePointsVisible NOTIFY samplePointsVisibleChanged)

public:
    explicit WaveformEditorItem(QQuickItem *parent = nullptr);

    QString sourcePath() const;
    void setSourcePath(const QString &value);
    QByteArray overviewData() const;
    void setOverviewData(const QByteArray &value);
    double positionSeconds() const;
    void setPositionSeconds(double value);
    double durationSeconds() const;
    void setDurationSeconds(double value);
    double zoomLevel() const;
    void setZoomLevel(double value);
    bool playing() const;
    void setPlaying(bool value);
    bool zoomEnabled() const;
    void setZoomEnabled(bool value);
    bool gridEnabled() const;
    void setGridEnabled(bool value);
    bool crosshairEnabled() const;
    void setCrosshairEnabled(bool value);
    bool showFpsOverlay() const;
    void setShowFpsOverlay(bool value);
    int viewMode() const;
    void setViewMode(int value);
    qulonglong mutedChannelsMask() const;
    void setMutedChannelsMask(qulonglong value);
    int soloedChannel() const;
    void setSoloedChannel(int value);
    int channelCountHint() const;
    void setChannelCountHint(int value);
    int channelCount() const;
    int sampleRateHz() const;
    bool samplePointsVisible() const;

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE double maximumZoomLevel() const;
    Q_INVOKABLE void setHoverPosition(double x, double y, bool active);

    void paint(QPainter *painter) override;

signals:
    void sourcePathChanged();
    void overviewDataChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void zoomLevelChanged();
    void playingChanged();
    void zoomEnabledChanged();
    void gridEnabledChanged();
    void crosshairEnabledChanged();
    void showFpsOverlayChanged();
    void viewModeChanged();
    void mutedChannelsMaskChanged();
    void soloedChannelChanged();
    void channelCountHintChanged();
    void channelCountChanged();
    void sampleRateHzChanged();
    void samplePointsVisibleChanged();
    void seekRequested(double seconds);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverEnterEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct DetailWindow {
        int sampleRateHz{0};
        int channelCount{0};
        double startSeconds{0.0};
        double endSeconds{0.0};
        quint32 framesPerPoint{0};
        int pointCount{0};
        std::vector<float> extrema;
    };

    static QByteArray decodeWindow(
        const QString &path, double startSeconds, double endSeconds, int maxPoints);
    static bool parseWindow(const QByteArray &bytes, DetailWindow *window);
    void scheduleDetailRequest();
    void requestDetailWindow();
    void clearDetailLocked();
    void clearPendingRequestLocked();
    double displayedPositionSecondsLocked() const;
    bool detailOrPendingRequestCoversLocked(double startSeconds, double endSeconds) const;
    bool detailCoversRangeLocked(double startSeconds, double endSeconds) const;
    bool detailResolutionCoversLocked(double startSeconds, double endSeconds) const;
    std::pair<double, double> visibleRangeLocked() const;
    std::pair<double, double> requestRangeLocked(
        double visibleStart, double visibleEnd) const;
    int detailRequestPointCountLocked(double requestStart, double requestEnd) const;
    int renderPixelWidthLocked() const;
    double maximumZoomLevelLocked() const;
    bool samplePointsVisibleLocked() const;
    void updateFpsEstimateLocked();
    void bindWindowFrameLoop(QQuickWindow *window);
    void handleWindowFrameSwapped();
    void invalidateCacheLocked();
    void rebuildCacheLocked(int width, int height);
    void drawGridLocked(QPainter &painter, int width, int height,
                        double visibleStart, double visibleEnd, int channels) const;
    void drawOverviewLocked(QPainter &painter, int width, int height,
                            double visibleStart, double visibleEnd, int channels) const;
    void drawDetailLocked(QPainter &painter, int width, int height,
                          double visibleStart, double visibleEnd, int channels) const;
    void drawCrosshair(QPainter &painter, int width, int height,
                       double visibleStart, double visibleEnd) const;
    static void drawFpsOverlay(QPainter &painter, int width, int fpsValue);
    int displayedChannelCountLocked() const;
    bool channelIsMutedLocked(int channel) const;

    mutable QMutex m_stateMutex;
    QString m_sourcePath;
    QByteArray m_overviewData;
    double m_positionSeconds{0.0};
    std::chrono::steady_clock::time_point m_positionUpdatedAt;
    double m_durationSeconds{0.0};
    double m_zoomLevel{1.0};
    bool m_playing{false};
    bool m_zoomEnabled{true};
    bool m_gridEnabled{false};
    bool m_crosshairEnabled{false};
    bool m_showFpsOverlay{false};
    int m_viewMode{0};
    qulonglong m_mutedChannelsMask{0};
    int m_soloedChannel{-1};
    int m_channelCountHint{1};
    int m_channelCount{0};
    int m_sampleRateHz{0};
    bool m_hoverActive{false};
    QPointF m_hoverPosition;
    DetailWindow m_detail;
    int m_detailRenderWidth{0};
    QImage m_cache;
    bool m_cacheDirty{true};
    int m_cachedViewportWidth{0};
    int m_cachedViewportHeight{0};
    double m_cacheStartSeconds{0.0};
    double m_cacheEndSeconds{0.0};
    quint64 m_requestGeneration{0};
    bool m_requestInFlight{false};
    double m_requestedStartSeconds{0.0};
    double m_requestedEndSeconds{0.0};
    int m_requestedMaxPoints{0};
    int m_requestedRenderWidth{0};
    bool m_fpsInitialized{false};
    int m_fpsValue{0};
    int m_fpsAccumFrames{0};
    double m_fpsAccumSeconds{0.0};
    std::chrono::steady_clock::time_point m_lastFrameTime;
    QMetaObject::Connection m_frameSwappedConnection;
    QMetaObject::Connection m_windowVisibilityConnection;
    QTimer m_requestTimer;
};
