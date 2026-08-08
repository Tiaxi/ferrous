// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QPointF>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>

#include <vector>

class QHoverEvent;
class QMouseEvent;
class QPainter;
class QWheelEvent;

class WaveformEditorItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QByteArray overviewData READ overviewData WRITE setOverviewData NOTIFY overviewDataChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
    Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled WRITE setGridEnabled NOTIFY gridEnabledChanged)
    Q_PROPERTY(bool crosshairEnabled READ crosshairEnabled WRITE setCrosshairEnabled NOTIFY crosshairEnabledChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(qulonglong mutedChannelsMask READ mutedChannelsMask WRITE setMutedChannelsMask NOTIFY mutedChannelsMaskChanged)
    Q_PROPERTY(int soloedChannel READ soloedChannel WRITE setSoloedChannel NOTIFY soloedChannelChanged)
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
    bool zoomEnabled() const;
    void setZoomEnabled(bool value);
    bool gridEnabled() const;
    void setGridEnabled(bool value);
    bool crosshairEnabled() const;
    void setCrosshairEnabled(bool value);
    int viewMode() const;
    void setViewMode(int value);
    qulonglong mutedChannelsMask() const;
    void setMutedChannelsMask(qulonglong value);
    int soloedChannel() const;
    void setSoloedChannel(int value);
    int channelCount() const;
    int sampleRateHz() const;
    bool samplePointsVisible() const;

    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE double maximumZoomLevel() const;

    void paint(QPainter *painter) override;

signals:
    void sourcePathChanged();
    void overviewDataChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void zoomLevelChanged();
    void zoomEnabledChanged();
    void gridEnabledChanged();
    void crosshairEnabledChanged();
    void viewModeChanged();
    void mutedChannelsMaskChanged();
    void soloedChannelChanged();
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
    std::pair<double, double> visibleRangeLocked() const;
    double maximumZoomLevelLocked() const;
    bool samplePointsVisibleLocked() const;
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
    int displayedChannelCountLocked() const;
    bool channelIsMutedLocked(int channel) const;

    mutable QMutex m_stateMutex;
    QString m_sourcePath;
    QByteArray m_overviewData;
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    double m_zoomLevel{1.0};
    bool m_zoomEnabled{true};
    bool m_gridEnabled{false};
    bool m_crosshairEnabled{false};
    int m_viewMode{0};
    qulonglong m_mutedChannelsMask{0};
    int m_soloedChannel{-1};
    int m_channelCount{1};
    int m_sampleRateHz{0};
    bool m_hoverActive{false};
    QPointF m_hoverPosition;
    DetailWindow m_detail;
    QImage m_cache;
    bool m_cacheDirty{true};
    int m_cachedViewportWidth{0};
    int m_cachedViewportHeight{0};
    double m_cacheStartSeconds{0.0};
    double m_cacheEndSeconds{0.0};
    quint64 m_requestGeneration{0};
    QTimer m_requestTimer;
};
