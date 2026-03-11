#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "TagEditorTableModel.h"

class BridgeClient;

class TagEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* tableModel READ tableModel CONSTANT)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString statusDetails READ statusDetails NOTIFY statusChanged)

public:
    explicit TagEditorController(BridgeClient *bridge, QObject *parent = nullptr);

    QObject *tableModel() const;
    bool isOpen() const;
    bool loading() const;
    bool saving() const;
    bool dirty() const;
    QString statusText() const;
    QString statusDetails() const;

    Q_INVOKABLE bool openSelection(const QVariantList &selections);
    Q_INVOKABLE bool openForPaths(const QStringList &paths);
    Q_INVOKABLE void close();
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool renameSelectedFiles();
    Q_INVOKABLE void setSelectedRows(const QVariantList &rows);
    Q_INVOKABLE QVariantList selectedRows() const;
    Q_INVOKABLE QString bulkValue(const QString &field) const;
    Q_INVOKABLE void applyBulkField(const QString &field, const QString &value);
    Q_INVOKABLE void applyBulkFieldToRows(
        const QVariantList &rows,
        const QString &field,
        const QString &value);
    Q_INVOKABLE void setCell(int row, const QString &field, const QString &value);
    Q_INVOKABLE void applyEnglishTitleCase(const QString &field);
    Q_INVOKABLE void applyFinnishCapitalize(const QString &field);
    Q_INVOKABLE void applyGenreCapitalize();
    Q_INVOKABLE void autoNumber(
        int startingTrack,
        int startingDisc,
        bool writeDiscNumbers,
        bool writeTotals,
        bool resetOnFolder,
        bool resetOnDiscChange);
    Q_INVOKABLE QStringList loadedPaths() const;

signals:
    void openChanged();
    void loadingChanged();
    void savingChanged();
    void dirtyChanged();
    void statusChanged();
    void selectionChanged();
    void bulkSummaryChanged();

private:
    QVector<int> targetRows() const;
    void setStatusText(const QString &text, const QString &details = QString());
    void resetSelection();
    void notifyRowMutation();

    BridgeClient *m_bridge{nullptr};
    TagEditorTableModel m_tableModel;
    QSet<int> m_selectedRows;
    QStringList m_loadedPaths;
    QString m_statusText;
    QString m_statusDetails;
    bool m_open{false};
    bool m_loading{false};
    bool m_saving{false};
};
