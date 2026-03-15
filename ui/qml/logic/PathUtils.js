.pragma library

function urlToLocalPath(urlValue) {
    if (urlValue === undefined || urlValue === null) {
        return ""
    }
    let text = String(urlValue)
    if (text.length === 0 || text === "undefined" || text === "null") {
        return ""
    }
    if (text.startsWith("QUrl(\"") && text.endsWith("\")")) {
        text = text.substring(6, text.length - 2)
    }
    if (text.indexOf("file://") !== 0) {
        return text
    }
    try {
        return decodeURIComponent(text.replace(/^file:\/\//, ""))
    } catch (error) {
        return text.replace(/^file:\/\//, "")
    }
}

function pathFromAnyUrl(urlValue) {
    const text = (urlValue || "").toString()
    if (text.length === 0) {
        return ""
    }
    const localPath = text.indexOf("file://") === 0 ? urlToLocalPath(text) : text
    const queryIndex = localPath.indexOf("?")
    const fragmentIndex = localPath.indexOf("#")
    let endIndex = localPath.length
    if (queryIndex >= 0) {
        endIndex = Math.min(endIndex, queryIndex)
    }
    if (fragmentIndex >= 0) {
        endIndex = Math.min(endIndex, fragmentIndex)
    }
    return endIndex < localPath.length ? localPath.substring(0, endIndex) : localPath
}

function folderDialogPath(dialogObj) {
    if (!dialogObj) {
        return ""
    }
    const candidates = [dialogObj.folder, dialogObj.selectedFolder, dialogObj.currentFolder]
    for (let i = 0; i < candidates.length; ++i) {
        const pathValue = urlToLocalPath(candidates[i])
        if (pathValue.length > 0) {
            return pathValue
        }
    }
    return ""
}

function fileDialogPaths(dialogObj) {
    if (!dialogObj) {
        return []
    }
    const paths = []
    const candidates = [
        dialogObj.files,
        dialogObj.selectedFiles,
        dialogObj.currentFiles,
        dialogObj.file,
        dialogObj.selectedFile,
        dialogObj.currentFile
    ]
    for (let i = 0; i < candidates.length; ++i) {
        const candidate = candidates[i]
        if (candidate === undefined || candidate === null) {
            continue
        }
        if (candidate.length !== undefined && typeof candidate !== "string") {
            for (let j = 0; j < candidate.length; ++j) {
                const pathValue = urlToLocalPath(candidate[j])
                if (pathValue.length > 0) {
                    paths.push(pathValue)
                }
            }
            if (paths.length > 0) {
                return paths
            }
            continue
        }
        const pathValue = urlToLocalPath(candidate)
        if (pathValue.length > 0) {
            paths.push(pathValue)
        }
    }
    return paths
}

function droppedExternalPaths(drop) {
    if (!drop) {
        return []
    }
    const paths = []
    const urls = drop.urls || []
    for (let i = 0; i < urls.length; ++i) {
        const pathValue = pathFromAnyUrl(urls[i])
        if (pathValue.length > 0 && paths.indexOf(pathValue) < 0) {
            paths.push(pathValue)
        }
    }
    if (paths.length > 0) {
        return paths
    }
    if (drop.hasText && (drop.text || "").length > 0) {
        const lines = (drop.text || "").split(/\r?\n/)
        for (let i = 0; i < lines.length; ++i) {
            const pathValue = urlToLocalPath(lines[i])
            if (pathValue.length > 0 && paths.indexOf(pathValue) < 0) {
                paths.push(pathValue)
            }
        }
    }
    return paths
}
