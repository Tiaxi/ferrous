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
    let text = (urlValue || "").toString()
    if (text.length === 0) {
        return ""
    }
    if (text.startsWith("QUrl(\"") && text.endsWith("\")")) {
        text = text.substring(6, text.length - 2)
    }
    const coversPrefix = "image://covers/"
    const localPath = text.indexOf("file://") === 0
        ? urlToLocalPath(text)
        : (text.indexOf(coversPrefix) === 0 ? text.substring(coversPrefix.length) : text)
    const queryIndex = localPath.indexOf("?")
    const fragmentIndex = localPath.indexOf("#")
    let endIndex = localPath.length
    if (queryIndex >= 0) {
        endIndex = Math.min(endIndex, queryIndex)
    }
    if (fragmentIndex >= 0) {
        endIndex = Math.min(endIndex, fragmentIndex)
    }
    const path = endIndex < localPath.length ? localPath.substring(0, endIndex) : localPath
    if (text.indexOf(coversPrefix) !== 0) {
        return path
    }
    try {
        return decodeURIComponent(path)
    } catch (error) {
        return path
    }
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

// `path` on at most two lines of `width`, as `metrics` (a FontMetrics)
// measures it: whole on one line where it fits; otherwise broken after the
// last "/" that fits the first line, or, where that would leave the line
// under half full, at the last space that does, or after the last
// character that does. The rest is the second line, elided in its middle
// where it is still too long, so that the path's start and its last
// folder's name stay.
function pathOnTwoLines(path, width, metrics) {
    if (path.length === 0 || width <= 0 || metrics.advanceWidth(path) <= width) {
        return path
    }
    // The longest start of the path that fits, in characters.
    let fits = 0
    while (fits < path.length && metrics.advanceWidth(path.substring(0, fits + 1)) <= width) {
        ++fits
    }
    fits = Math.max(1, fits)
    const lastBefore = function(character) {
        // A "/" stays on the first line, a space ends it.
        const at = path.lastIndexOf(character, character === "/" ? fits - 1 : fits)
        return at > 0 ? at + 1 : 0
    }
    const halfFull = function(cut) {
        return cut > 0 && metrics.advanceWidth(path.substring(0, cut)) >= width / 2
    }
    let cut = lastBefore("/")
    if (!halfFull(cut)) {
        cut = lastBefore(" ")
    }
    if (!halfFull(cut)) {
        cut = fits
    }
    return path.substring(0, cut) + "\n" + metrics.elidedText(path.substring(cut), Qt.ElideMiddle, width)
}
