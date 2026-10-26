// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <functional>
#include <memory>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "TagEditorTableModel.h"

class BridgeClient;
class QThreadPool;
struct FerrousFfiRenameGate;

class TagEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* tableModel READ tableModel CONSTANT)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString statusDetails READ statusDetails NOTIFY statusChanged)
    // Whether the status reports something that failed.
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // The names of the files the running save or rename writes.
    Q_PROPERTY(QStringList operationFiles READ operationFiles NOTIFY savingChanged)

public:
    explicit TagEditorController(BridgeClient *bridge, QObject *parent = nullptr);
    ~TagEditorController() override;

    QObject *tableModel() const;
    bool isOpen() const;
    bool loading() const;
    bool saving() const;
    bool dirty() const;
    QString statusText() const;
    QString statusDetails() const;
    bool statusIsError() const;
    int selectedCount() const;
    QStringList operationFiles() const;

    Q_INVOKABLE bool openSelection(const QVariantList &selections);
    Q_INVOKABLE bool openForPaths(const QStringList &paths);
    Q_INVOKABLE void close();
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool renameSelectedFiles();
    // Work out, without renaming anything, what renameSelectedFiles would
    // do, and answer with renamePreviewReady.
    Q_INVOKABLE bool previewRenameSelectedFiles();
    // Rename the files of the latest preview, from the rows as they were
    // previewed, whatever is selected or edited since. Returns false, and
    // says so in the status, when that preview is gone, such as after the
    // files were loaded again.
    Q_INVOKABLE bool renamePreviewedFiles();
    Q_INVOKABLE void setSelectedRows(const QVariantList &rows);
    Q_INVOKABLE QVariantList selectedRows() const;
    // Selects rows `first` to `last`, in either order, and no others.
    Q_INVOKABLE void selectRange(int first, int last);
    Q_INVOKABLE void toggleRowSelected(int row);
    Q_INVOKABLE void selectAllRows();
    Q_INVOKABLE QString bulkValue(const QString &field) const;
    // Says which of the edited rows hold more than one value of `field`,
    // which an edit replaces; empty when none do.
    Q_INVOKABLE QString extraValuesNote(const QString &field) const;
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
    void saveFinished(bool success);
    void renameFinished();
    // A load, save, or rename that failed, in whole or in part: the status
    // and the first failures, for the app's error list.
    void errorReported(const QString &message);
    // Each row has the file's oldName and newName, its path, newPath (empty
    // if it has none), and whether it would be renamed (`ok`, with
    // `unchanged` for a name that stays) or why not (`error`). `error` is
    // set instead when the preview failed as a whole.
    void renamePreviewReady(const QVariantList &rows, const QString &error);

private:
    enum class Operation { Load, Save, Rename, PreviewRename };
    using RenameGate = std::shared_ptr<FerrousFfiRenameGate>;
    static QByteArray executeOperation(Operation operation, const QByteArray &payload,
                                       const RenameGate &renameGate);
    void runOperation(Operation operation, const QByteArray &payload,
                      std::function<void(const QByteArray &)> apply);
    // A load response, parsed by the worker so thousands of rows never
    // block the UI thread.
    struct LoadResult {
        QVector<TagEditorRowState> rows;
        QStringList resolvedPaths;
        QString errorText;
        QStringList failureLines;
    };
    static LoadResult parseLoadResponse(const QByteArray &response);
    void runLoad(const QByteArray &payload);
    void applyLoadResult(LoadResult result);
    void applySaveResult(const QByteArray &response);
    void applyRenameResult(const QByteArray &response);
    void applyRenamePreview(const QByteArray &response);
    // The rename request for the selected rows and their file names; empty
    // when no row is selected.
    QByteArray buildRenameRequest(QStringList *fileNames) const;
    // A rename request and the names of its files: the one a preview runs,
    // then the one it answered, which renamePreviewedFiles renames.
    struct RenameRequest {
        QByteArray payload;
        QStringList fileNames;
    };
    RenameRequest m_pendingRenamePreview;
    RenameRequest m_renamePreview;
    bool startRename(RenameRequest request);
    std::function<QByteArray(Operation, const QByteArray &, const RenameGate &)> m_executeOperation{
        executeOperation};
    quint64 m_operationGeneration{0};
    // How long destruction waits for a running file operation. One stalled
    // on unresponsive storage is left running, so it cannot hold up the exit.
    static constexpr int kExitWaitMs = 1000;
    // The workers' own pool: the application waits for its global pool at
    // exit for as long as its work takes.
    QThreadPool *m_workerPool{nullptr};
    QStringList m_operationFiles;
    QVector<int> targetRows() const;
    void reportFailures(const QString &status, const QStringList &failureLines);
    void setStatusText(const QString &text, const QString &details = QString(),
                       bool isError = false);
    void setSelection(QSet<int> rows);
    void resetSelection();
    void notifyRowMutation();

    BridgeClient *m_bridge{nullptr};
    TagEditorTableModel m_tableModel;
    QSet<int> m_selectedRows;
    QStringList m_loadedPaths;
    QString m_statusText;
    QString m_statusDetails;
    bool m_statusIsError{false};
    bool m_open{false};
    bool m_loading{false};
    bool m_saving{false};
};
