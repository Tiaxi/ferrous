.pragma library

// Where wrapped text that holds a path or a system error may break: a path
// between its folders, never inside a folder's name, and an error code such
// as "(os error 13)" never at all. The text shown carries characters that
// only shape its lines; its accessible name, its tooltip and what is copied
// keep the text as it is (plainText gives it back).

const zeroWidthSpace = "\u200b"
const wordJoiner = "\u2060"
const noBreakSpace = "\u00a0"

// A name kept on one line: its spaces no-break spaces, and a word joiner
// after each hyphen or dash, after which a line may otherwise break.
function unbreakable(name) {
    return name.replace(/ /g, noBreakSpace).replace(/[-\u2010\u2012\u2013\u2014]/g, "$&" + wordJoiner)
}

// A path whose lines break only between folders: a zero-width space after
// each slash is a break point, and each name that `fits` on a line (called
// with the name and its slash) is kept whole ("…/2005 - Small"
// broke before "Hours"). A name wider than the line breaks between its
// words.
function breakablePath(path, fits) {
    return String(path || "").split("/").map(function(name) {
        return fits(name + "/") ? unbreakable(name) : name
    }).join("/" + zeroWidthSpace)
}

// A path in a message: from a slash, or "~/", at the start of a word or
// after an opening bracket or quote, to the next line, double quote, or
// colon, comma, semicolon or full stop before a space or the text's end.
// Its names may hold spaces and apostrophes.
const messagePath = /(^|[\s(\[\u201c\u2018"'])(~?\/(?:[^\n"\u201c\u201d:,;.]|[:,;.](?!\s|$))*)/g
// A system error's code, as Rust gives it.
const osError = /\(os error (\d+)\)/g

// A message whose lines break inside a path only between its folders (see
// breakablePath), and never inside an error code such as "(os error 13)"
// ("13)" was alone on a line). A path is not broken right after its
// first slash: the space before it is the break.
function breakableMessage(text, fits) {
    return String(text || "")
        .replace(messagePath, function(match, before, path) {
            const trimmed = path.replace(/\s+$/, "")
            return before + breakablePath(trimmed, fits).replace("/" + zeroWidthSpace, "/")
                + path.substring(trimmed.length)
        })
        .replace(osError, "(os" + noBreakSpace + "error" + noBreakSpace + "$1)")
}

// Text as it was before breakablePath or breakableMessage, for the
// clipboard.
function plainText(text) {
    return String(text || "").split(zeroWidthSpace).join("").split(wordJoiner).join("")
        .replace(/\u00a0/g, " ")
}
