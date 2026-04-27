Name:           inputflow
Version:        0.1.0
Release:        1%{?dist}
Summary:        Linux client for PowerToys Mouse Without Borders

License:        MIT
URL:            https://github.com/daredoole/mwb-client-linux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  openssl-devel
BuildRequires:  pkgconf-pkg-config
BuildRequires:  systemd-rpm-macros
BuildRequires:  zlib-devel

Requires:       systemd
%{?systemd_requires}
Requires(pre):  systemd
Requires(post): systemd-udev
Requires(postun): systemd-udev
Recommends:     wl-clipboard

%description
InputFlow is a Linux companion client for Microsoft PowerToys Mouse
Without Borders. The current command, configuration path, and user service
retain the mwb-client naming for compatibility.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake
%cmake_build

%install
%cmake_build --target mwb_client
install -Dpm0755 %{_vpath_builddir}/mwb_client %{buildroot}%{_bindir}/mwb_client
install -Dpm0644 packaging/usr/lib/sysusers.d/mwb-client.conf %{buildroot}%{_sysusersdir}/mwb-client.conf
install -Dpm0644 packaging/usr/lib/modules-load.d/mwb-client-uinput.conf %{buildroot}%{_modulesloaddir}/mwb-client-uinput.conf
install -Dpm0644 packaging/usr/lib/udev/rules.d/70-mwb-client-uinput.rules %{buildroot}%{_udevrulesdir}/70-mwb-client-uinput.rules
install -Dpm0644 packaging/usr/lib/systemd/user/mwb-client.service %{buildroot}%{_userunitdir}/mwb-client.service

%pre
%sysusers_create %{_sysusersdir}/mwb-client.conf

%post
%systemd_user_post mwb-client.service
udevadm control --reload-rules || :
udevadm trigger --name-match=uinput || :

%preun
%systemd_user_preun mwb-client.service

%postun
%systemd_user_postun_with_restart mwb-client.service
udevadm control --reload-rules || :
udevadm trigger --name-match=uinput || :

%files
%license LICENSE
%doc README.md packaging/README.md
%{_bindir}/mwb_client
%{_sysusersdir}/mwb-client.conf
%{_modulesloaddir}/mwb-client-uinput.conf
%{_udevrulesdir}/70-mwb-client-uinput.rules
%{_userunitdir}/mwb-client.service

%changelog
* Fri Apr 24 2026 InputFlow Maintainers <maintainers@example.invalid> - 0.1.0-1
- Add initial Fedora/RPM packaging skeleton.
