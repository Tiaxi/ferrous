#pragma once

#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QQuickPaintedItem>
#include <QByteArray>
#include <QVariantList>

#include <array>
#include <chrono>
#include <deque>
#include <vector>

class QQuickWindow;

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
    static constexpr int kGradientTableSize = 2048;

    void rebuildPalette();
    void invalidateMapping();
    void ensureMapping(int height);
    void invalidateCanvas();
    void ensureCanvas(int width, int height);
    void rebuildCanvasFromColumns();
    void shiftCanvasLeft(int columns);
    void drawColumnAt(int x, const std::vector<quint8> &col);
    void appendColumnAndRender(std::vector<quint8> &&col);
    std::vector<quint8> rowToIntensity(const QVariantList &row) const;
    void bindWindowFpsTracking(QQuickWindow *window);
    void handleWindowFrameSwapped();
    void updateFpsEstimateLocked();
    void drawFpsOverlay(QPainter *painter) const;

    double m_dbRange{90.0};
    bool m_logScale{false};
    int m_sampleRateHz{48000};
    int m_maxColumns{640};
    int m_binsPerColumn{0};

    std::array<QRgb, kGradientTableSize> m_palette32{};
    std::vector<int> m_iToBin;
    int m_mappingHeight{-1};
    int m_lowResEnd{0};

    QImage m_canvas;
    bool m_canvasDirty{true};
    std::deque<std::vector<quint8>> m_columns;
    bool m_showFpsOverlay{false};
    bool m_fpsInitialized{false};
    int m_fpsValue{0};
    int m_fpsAccumFrames{0};
    double m_fpsAccumSeconds{0.0};
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool m_profileEnabled{false};
    std::chrono::steady_clock::time_point m_profileLast{};
    quint64 m_profilePaints{0};
    double m_profilePaintMs{0.0};
    QMetaObject::Connection m_frameSwapConnection;
    mutable QMutex m_stateMutex;
};
