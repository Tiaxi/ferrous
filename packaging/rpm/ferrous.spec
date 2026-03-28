%{!?ferrous_version:%global ferrous_version 0.1.0}
%{!?ferrous_release:%global ferrous_release 0.local}
%{!?ferrous_license:%global ferrous_license GPL-3.0-or-later}

Name:           ferrous
Version:        %{ferrous_version}
Release:        %{ferrous_release}
Summary:        A fast, Linux-native desktop music player
License:        %{ferrous_license}
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  cargo
BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  rust
BuildRequires:  kf6-kirigami-devel
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(gstreamer-audio-1.0)
BuildRequires:  pkgconfig(gstreamer-pbutils-1.0)

%description
Ferrous is a fast, Linux-native desktop music player with a Qt6/Kirigami
frontend and a Rust backend.

%prep
%autosetup -n %{name}-%{version}

%build
cmake -S ui -B ui/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=%{_prefix}
cmake --build ui/build --parallel %{?_smp_build_ncpus}

%check
ctest --test-dir ui/build --output-on-failure

%install
DESTDIR=%{buildroot} cmake --install ui/build

%posttrans
update-desktop-database %{_datadir}/applications >/dev/null 2>&1 || :

%postun
update-desktop-database %{_datadir}/applications >/dev/null 2>&1 || :

%files
%doc README.md
%{_bindir}/ferrous
%{_datadir}/applications/ferrous.desktop
%{_datadir}/icons/hicolor/scalable/apps/ferrous.svg

%changelog
* Fri Mar 28 2026 Ferrous contributors - %{version}-%{release}
- Initial RPM packaging
