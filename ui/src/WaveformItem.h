// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QMutex>
#include <QQuickPaintedItem>

#include <chrono>

class WaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QByteArray peaksData READ peaksData WRITE setPeaksData NOTIFY peaksDataChanged)
    Q_PROPERTY(double generatedSeconds READ generatedSeconds WRITE setGeneratedSeconds NOTIFY generatedSecondsChanged)
    Q_PROPERTY(bool waveformComplete READ waveformComplete WRITE setWaveformComplete NOTIFY waveformCompleteChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(QColor barColor READ barColor WRITE setBarColor NOTIFY barColorChanged)
    Q_PROPERTY(QColor bgColor READ bgColor WRITE setBgColor NOTIFY bgColorChanged)

public:
    explicit WaveformItem(QQuickItem *parent = nullptr);

    QByteArray peaksData() const;
    void setPeaksData(const QByteArray &data);

    double generatedSeconds() const;
    void setGeneratedSeconds(double value);

    bool waveformComplete() const;
    void setWaveformComplete(bool value);

    double positionSeconds() const;
    void setPositionSeconds(double value);

    double durationSeconds() const;
    void setDurationSeconds(double value);

    QColor barColor() const;
    void setBarColor(const QColor &color);

    QColor bgColor() const;
    void setBgColor(const QColor &color);

    void paint(QPainter *painter) override;

signals:
    void peaksDataChanged();
    void generatedSecondsChanged();
    void waveformCompleteChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void barColorChanged();
    void bgColorChanged();

private:
    int currentWidthLocked() const;
    int currentHeightLocked() const;
    int drawnWidthLocked(double generatedSeconds, bool waveformComplete, double durationSeconds) const;
    int xForPeakIndexLocked(int peakIndex, int peakCount, int drawWidth) const;
    void ensureCacheLocked(int width, int height);
    void markDirtyRangeLocked(int x0, int x1);
    void markDirtyAllLocked();
    void updateWaveformCacheLocked();

    mutable QMutex m_stateMutex;
    QByteArray m_peaksData;
    double m_generatedSeconds{0.0};
    bool m_waveformComplete{false};
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    QImage m_waveformCache;
    QRect m_dirtyRect;
    QColor m_barColor{0x0f, 0x2e, 0x5d};
    QColor m_bgColor{Qt::white};
    bool m_cacheDirty{true};
    bool m_profileEnabled{false};
    std::chrono::steady_clock::time_point m_profileLast{};
    quint64 m_profilePaints{0};
    double m_profilePaintMs{0.0};
};
