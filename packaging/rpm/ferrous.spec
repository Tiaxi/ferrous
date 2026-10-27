%{!?ferrous_version:%global ferrous_version 0.2.2}
%{!?ferrous_release:%global ferrous_release 0.local%{?dist}}
%{!?ferrous_license:%global ferrous_license GPL-3.0-or-later}
# scripts/build-rpm.sh passes the date of the packaged commit.
%{!?ferrous_changelog_date:%global ferrous_changelog_date %(LC_ALL=C date -u +"%%a %%b %%d %%Y")}

Name:           ferrous
Version:        %{ferrous_version}
Release:        %{ferrous_release}
Summary:        A fast, Linux-native desktop music player
License:        %{ferrous_license}
URL:            https://github.com/Tiaxi/ferrous
Source0:        %{name}-%{version}.tar.gz
# Crates from Cargo.lock, made with `cargo vendor` by scripts/build-rpm.sh, so
# that %%build needs no network access.
Source1:        %{name}-%{version}-vendor.tar.gz

BuildRequires:  cargo
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  rust
BuildRequires:  kf6-kirigami-devel
# The Secret Service keyring links the system libdbus.
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(gstreamer-audio-1.0)
BuildRequires:  pkgconfig(gstreamer-pbutils-1.0)
BuildRequires:  appstream
BuildRequires:  desktop-file-utils

Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Requires:       qt6-qtquickcontrols2
Requires:       kf6-kirigami
# The org.kde.desktop controls style, which Ferrous selects at startup; without
# it, for example outside Plasma, the main window fails to load.
Requires:       kf6-qqc2-desktop-style
Requires:       gstreamer1
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Requires:       gstreamer1-plugins-bad-free
# AC-3 and DTS decoders (avdec_ac3, avdec_dca); the plugins above cover the
# other supported formats.
Recommends:     gstreamer1-plugin-libav

%description
Ferrous is a fast, Linux-native desktop music player with a Qt6/Kirigami
frontend and a Rust backend.

%prep
%autosetup -n %{name}-%{version} -a 1
mkdir -p .cargo
cat > .cargo/config.toml <<'EOF'
[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "vendor"

[net]
offline = true
EOF

%build
# Warnings stay errors for developer and CI builds, but a newer distribution
# compiler must not fail the package build.
cmake -S ui -B ui/build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DFERROUS_WARNINGS_AS_ERRORS=OFF \
    -DFERROUS_CARGO_ARGS=--frozen
cmake --build ui/build --parallel %{?_smp_build_ncpus}

%install
DESTDIR=%{buildroot} cmake --install ui/build

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/ferrous.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/io.github.tiaxi.ferrous.metainfo.xml
# Keep the tests away from the builder's own settings, library, and caches.
test_state="$(mktemp -d "$PWD/ui/build/test-state.XXXXXX")"
mkdir -p "${test_state}/tmp"
TMPDIR="${test_state}/tmp" \
XDG_CONFIG_HOME="${test_state}/config" \
XDG_DATA_HOME="${test_state}/data" \
XDG_CACHE_HOME="${test_state}/cache" \
    ctest --test-dir ui/build --output-on-failure
rm -rf "${test_state}"

%posttrans
update-desktop-database %{_datadir}/applications >/dev/null 2>&1 || :

%postun
update-desktop-database %{_datadir}/applications >/dev/null 2>&1 || :

%files
%license LICENSE
%doc README.md
%{_bindir}/ferrous
%{_datadir}/applications/ferrous.desktop
%{_datadir}/icons/hicolor/scalable/apps/ferrous.svg
%{_metainfodir}/io.github.tiaxi.ferrous.metainfo.xml

%changelog
* %{ferrous_changelog_date} Ferrous contributors - %{version}-%{release}
- Package Ferrous %{version}; see %{url}/releases for changes
