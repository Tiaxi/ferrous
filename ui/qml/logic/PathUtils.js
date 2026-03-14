.pragma library

function urlToLocalPath(urlValue) {
    if (urlValue === undefined || urlValue === null) {
        return ""
    }
    const text = String(urlValue)
    if (text.length === 0) {
        return ""
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
    if (text.indexOf("file://") === 0) {
        return urlToLocalPath(text)
    }
    return text
}

function folderDialogPath(dialogObj) {
    if (!dialogObj || dialogObj.folder === undefined || dialogObj.folder === null) {
        return ""
    }
    return pathFromAnyUrl(dialogObj.folder)
}

function fileDialogPaths(dialogObj) {
    if (!dialogObj) {
        return []
    }
    const urls = dialogObj.files || dialogObj.selectedFiles || []
    const paths = []
    for (let i = 0; i < urls.length; ++i) {
        const pathValue = pathFromAnyUrl(urls[i])
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
    return paths
}
