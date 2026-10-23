// SPDX-License-Identifier: GPL-3.0-or-later

//! Per-user directories for Ferrous state.
//!
//! Every config, data, and cache path is resolved here. Test builds resolve
//! them under a per-process temporary root instead, so unit tests never read
//! or write the real user directories.

use std::ffi::OsString;
use std::path::PathBuf;

const APP_DIR_NAME: &str = "ferrous";

/// `$XDG_CONFIG_HOME/ferrous`, falling back to `~/.config/ferrous`.
pub(crate) fn config_dir() -> Option<PathBuf> {
    app_dir(BaseDir::Config)
}

/// `$XDG_DATA_HOME/ferrous`, falling back to `~/.local/share/ferrous`.
pub(crate) fn data_dir() -> Option<PathBuf> {
    app_dir(BaseDir::Data)
}

/// `$XDG_CACHE_HOME/ferrous`, falling back to `~/.cache/ferrous`.
pub(crate) fn cache_dir() -> Option<PathBuf> {
    app_dir(BaseDir::Cache)
}

/// The library database, which also holds the waveform cache.
pub(crate) fn library_db_path() -> anyhow::Result<PathBuf> {
    data_dir()
        .map(|dir| dir.join("library.sqlite3"))
        .ok_or_else(|| {
            anyhow::anyhow!("cannot locate the data directory: set HOME or XDG_DATA_HOME")
        })
}

#[derive(Debug, Clone, Copy)]
enum BaseDir {
    Config,
    Data,
    Cache,
}

impl BaseDir {
    fn xdg_var(self) -> &'static str {
        match self {
            Self::Config => "XDG_CONFIG_HOME",
            Self::Data => "XDG_DATA_HOME",
            Self::Cache => "XDG_CACHE_HOME",
        }
    }

    fn home_relative(self) -> &'static str {
        match self {
            Self::Config => ".config",
            Self::Data => ".local/share",
            Self::Cache => ".cache",
        }
    }
}

fn app_dir(kind: BaseDir) -> Option<PathBuf> {
    #[cfg(test)]
    let base = Some(test_root().join(kind.home_relative()));
    #[cfg(not(test))]
    let base = resolve_base(
        std::env::var_os(kind.xdg_var()),
        std::env::var_os("HOME"),
        kind.home_relative(),
    );
    base.map(|base| base.join(APP_DIR_NAME))
}

/// Applies the XDG base directory rules: an absolute `$XDG_*_HOME` wins, and
/// unset, empty, or relative values fall back to a directory under `$HOME`.
fn resolve_base(
    xdg_value: Option<OsString>,
    home: Option<OsString>,
    home_relative: &str,
) -> Option<PathBuf> {
    xdg_value
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
        .or_else(|| {
            home.map(PathBuf::from)
                .filter(|path| path.is_absolute())
                .map(|home| home.join(home_relative))
        })
}

#[cfg(test)]
pub(crate) fn test_root() -> PathBuf {
    std::env::temp_dir().join(format!("ferrous-test-{}", std::process::id()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn test_builds_resolve_every_directory_under_the_temp_root() {
        let root = test_root();
        assert!(root.starts_with(std::env::temp_dir()));
        let resolved = [
            config_dir(),
            data_dir(),
            cache_dir(),
            library_db_path().ok(),
        ];
        for path in resolved {
            let path = path.expect("test builds always resolve a directory");
            assert!(
                path.starts_with(&root),
                "{} escapes the test root {}",
                path.display(),
                root.display()
            );
        }
        assert_eq!(
            library_db_path().ok(),
            Some(root.join(".local/share/ferrous/library.sqlite3"))
        );
    }

    #[test]
    fn absolute_xdg_directory_takes_precedence_over_home() {
        for kind in [BaseDir::Config, BaseDir::Data, BaseDir::Cache] {
            let resolved = resolve_base(
                Some(OsString::from("/xdg/base")),
                Some(OsString::from("/home/user")),
                kind.home_relative(),
            );
            assert_eq!(
                resolved,
                Some(PathBuf::from("/xdg/base")),
                "{}",
                kind.xdg_var()
            );
        }
    }

    #[test]
    fn unset_empty_or_relative_xdg_directory_falls_back_to_home() {
        for xdg_value in [None, Some(""), Some("relative/dir")] {
            for (kind, expected) in [
                (BaseDir::Config, "/home/user/.config"),
                (BaseDir::Data, "/home/user/.local/share"),
                (BaseDir::Cache, "/home/user/.cache"),
            ] {
                let resolved = resolve_base(
                    xdg_value.map(OsString::from),
                    Some(OsString::from("/home/user")),
                    kind.home_relative(),
                );
                assert_eq!(
                    resolved,
                    Some(PathBuf::from(expected)),
                    "{} = {xdg_value:?}",
                    kind.xdg_var()
                );
            }
        }
    }

    #[test]
    fn missing_or_relative_home_without_xdg_directory_resolves_nothing() {
        assert_eq!(resolve_base(None, None, ".config"), None);
        assert_eq!(
            resolve_base(None, Some(OsString::from("relative-home")), ".config"),
            None
        );
    }

    /// Guards against new code resolving user directories on its own, which
    /// would bypass the test root above.
    #[test]
    fn user_directories_are_resolved_only_in_this_module() {
        let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let this_file = src.join("paths.rs");
        let mut offenders = Vec::new();
        let mut pending = vec![src];
        while let Some(dir) = pending.pop() {
            for entry in std::fs::read_dir(&dir).expect("read source directory") {
                let path = entry.expect("read source entry").path();
                if path.is_dir() {
                    pending.push(path);
                } else if path.extension().is_some_and(|ext| ext == "rs") && path != this_file {
                    let text = std::fs::read_to_string(&path).expect("read source file");
                    if text.contains("\"XDG_") || text.contains("\"HOME\"") {
                        offenders.push(path);
                    }
                }
            }
        }
        assert!(
            offenders.is_empty(),
            "resolve user directories through crate::paths instead: {offenders:?}"
        );
    }
}
