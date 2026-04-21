// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::OsStr;

fn parse_enabled(value: Option<&OsStr>) -> bool {
    value.is_some_and(|raw| {
        let normalized = raw.to_string_lossy().trim().to_ascii_lowercase();
        !normalized.is_empty()
            && normalized != "0"
            && normalized != "false"
            && normalized != "off"
            && normalized != "no"
    })
}

pub fn heartbeat_trace_enabled() -> bool {
    cfg!(feature = "profiling-logs")
        && parse_enabled(std::env::var_os("FERROUS_PROFILE_HEARTBEAT_TRACE").as_deref())
}

#[cfg(test)]
mod tests {
    use std::ffi::OsStr;

    use super::parse_enabled;

    #[test]
    fn profile_heartbeat_trace_defaults_off() {
        assert!(!parse_enabled(None));
        assert!(!parse_enabled(Some(OsStr::new("0"))));
        assert!(!parse_enabled(Some(OsStr::new("false"))));
        assert!(!parse_enabled(Some(OsStr::new("off"))));
    }

    #[test]
    fn profile_heartbeat_trace_requires_explicit_opt_in() {
        assert!(parse_enabled(Some(OsStr::new("1"))));
        assert!(parse_enabled(Some(OsStr::new("true"))));
        assert!(parse_enabled(Some(OsStr::new("yes"))));
    }
}
