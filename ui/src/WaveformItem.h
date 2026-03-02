#pragma once

#include <QByteArray>
#include <QQuickPaintedItem>

#include <chrono>

class WaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QByteArray peaksData READ peaksData WRITE setPeaksData NOTIFY peaksDataChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)

public:
    explicit WaveformItem(QQuickItem *parent = nullptr);

    QByteArray peaksData() const;
    void setPeaksData(const QByteArray &data);

    double positionSeconds() const;
    void setPositionSeconds(double value);

    double durationSeconds() const;
    void setDurationSeconds(double value);

    void paint(QPainter *painter) override;

signals:
    void peaksDataChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();

private:
    QByteArray m_peaksData;
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    bool m_profileEnabled{false};
    std::chrono::steady_clock::time_point m_profileLast{};
    quint64 m_profilePaints{0};
    double m_profilePaintMs{0.0};
};
