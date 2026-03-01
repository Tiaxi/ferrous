#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QByteArray>
#include <QVariantList>

#include <array>
#include <deque>
#include <vector>

class SpectrogramItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY dbRangeChanged)
    Q_PROPERTY(bool logScale READ logScale WRITE setLogScale NOTIFY logScaleChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(int maxColumns READ maxColumns WRITE setMaxColumns NOTIFY maxColumnsChanged)

public:
    explicit SpectrogramItem(QQuickItem *parent = nullptr);

    double dbRange() const;
    void setDbRange(double value);

    bool logScale() const;
    void setLogScale(bool value);

    int sampleRateHz() const;
    void setSampleRateHz(int value);

    int maxColumns() const;
    void setMaxColumns(int value);

    Q_INVOKABLE void reset();
    Q_INVOKABLE void appendRows(const QVariantList &rows);
    Q_INVOKABLE void appendPackedRows(const QByteArray &packedRows, int rowCount, int binsPerRow);

    void paint(QPainter *painter) override;

signals:
    void dbRangeChanged();
    void logScaleChanged();
    void sampleRateHzChanged();
    void maxColumnsChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void rebuildPalette();
    void invalidateMapping();
    void ensureMapping(int height);
    std::vector<quint8> rowToIntensity(const QVariantList &row) const;

    double m_dbRange{90.0};
    bool m_logScale{false};
    int m_sampleRateHz{48000};
    int m_maxColumns{640};
    int m_binsPerColumn{0};

    std::array<std::array<quint8, 3>, 256> m_palette{};
    std::vector<int> m_yToBin;
    int m_yToBinHeight{-1};

    std::deque<std::vector<quint8>> m_columns;
};
