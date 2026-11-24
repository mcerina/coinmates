#!/bin/bash
# The AppImage, for Linux without flatpak. Neither MAME nor EmuDeck has one: the flatpak's options,
# built as AppImages are, on the oldest system they run on: AlmaLinux 9 (glibc 2.34: SteamOS and the
# distributions since 2021-2022), with its gcc 14 toolset, whose programs need only the system's
# older C++ library. Not Ubuntu 22.04: its gcc 12 miscompiles bounds checks (a valid index of the
# NEC core's table checked with a register never set: airass stopped at once). It brings SDL 3,
# which these systems lack: SDL's releases, as the macOS builds. The rest comes from the system (C
# and C++ libraries, OpenGL, X11, fontconfig, FreeType, ALSA), as every desktop has it; a bundled
# one would clash with the graphics drivers.
# Run as root in docker.io/library/almalinux:9, in the repository; writes coinmates-x86_64.AppImage.
# Env: JOBS (default: all cores), CCACHE_DIR (default .ccache).
set -e
cd "$(dirname "$0")/../.."
# patchelf and ccache from EPEL, the -devel of Wayland and libdecor from CRB. SDL's libraries, from
# its README-linux (X11, Wayland, PulseAudio, PipeWire, udev for the controllers, D-Bus), only to
# build it: it loads them when there
dnf -y install epel-release dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -y install --setopt=install_weak_deps=False gcc-toolset-14-gcc-c++ make python3 git cmake pkgconf patchelf ccache librsvg2-tools file \
  fontconfig-devel freetype-devel alsa-lib-devel libX11-devel libXinerama-devel libXi-devel libXext-devel mesa-libGL-devel \
  libXrandr-devel libXcursor-devel libXScrnSaver-devel libXfixes-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libdecor-devel \
  mesa-libEGL-devel mesa-libgbm-devel libdrm-devel pulseaudio-libs-devel pipewire-devel systemd-devel dbus-devel
. /opt/rh/gcc-toolset-14/enable
export CC=gcc CXX=g++ CCACHE_DIR=${CCACHE_DIR:-$PWD/.ccache}
# the hardening of the flatpak's SDK (include/flags.yml of the Freedesktop SDK), for SDL too, as the
# runtime's, whatever the compiler's defaults; as the SDK's compiler, the baseline x86-64 and a PIE
# program (-pie below), where the toolset's would ask for x86-64-v2 and make a fixed-address one
export CFLAGS="-march=x86-64 -mtune=generic -fPIE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -fstack-clash-protection -fcf-protection=full"
export CXXFLAGS=$CFLAGS LDFLAGS="-Wl,-z,relro,-z,now"
get() {
  curl -sSfL "$1" -o /tmp/dl
  echo "$2  /tmp/dl" | sha256sum -c -
}
cmakebuild() {
  tar xf /tmp/dl -C /tmp
  cmake -S /tmp/$1 -B /tmp/$1/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64 -DCMAKE_C_COMPILER_LAUNCHER=ccache "${@:2}"
  cmake --build /tmp/$1/build -j${JOBS:-$(nproc)}
  cmake --install /tmp/$1/build
}
get https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
cmakebuild SDL3-3.4.18 -DSDL_TESTS=OFF
# no HarfBuzz nor SVG: MAME's menu needs neither
get https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz 63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d
cmakebuild SDL3_ttf-3.2.2 -DSDLTTF_HARFBUZZ=OFF -DSDLTTF_PLUTOSVG=OFF -DSDLTTF_SAMPLES=OFF
# the flatpak's make command (its manifest says why). Not the flatpak's: MAME's own FLAC, as Windows
# and macOS; no PulseAudio and PipeWire modules, as MAME plays through SDL first (the flatpak too),
# which loads them itself: their libraries in here would have to match the system's sound server
make -j${JOBS:-$(nproc)} BUILDDIR=build-appimage CFLAGS+=-Wno-error=restrict OSD=sdl3 NO_USE_PULSEAUDIO=1 NO_USE_PIPEWIRE=1 LDOPTS="-Wl,-s -pie" \
  SUBTARGET=coinmates SOURCES=$(grep -v '^#' netplay/sources.txt | paste -sd,) \
  USE_QTDEBUG=0 ARCHOPTS="-fsanitize=bounds -fsanitize-undefined-trap-on-error" NEW_GIT_VERSION=$(netplay/version.sh) \
  OVERRIDE_CC="ccache gcc" OVERRIDE_CXX="ccache g++"
netplay/validate.sh ./coinmates build-appimage
# the flatpak's files, at the same places under usr
a=/tmp/AppDir
rm -rf $a
install -Dm755 coinmates $a/usr/bin/coinmates
install -Dm755 netplay/flatpak/launch $a/usr/bin/launch
install -Dm644 netplay/flatpak/default.cfg $a/usr/share/coinmates/default.cfg
for f in COPYING 3rdparty/core-math/LICENSE:CORE-MATH-LICENSE 3rdparty/musl/COPYRIGHT:MUSL-COPYRIGHT netplay/flatpak/EMUDECK-LICENSE; do
  install -Dm644 ${f%%:*} $a/usr/share/licenses/coinmates/$(basename ${f#*:})
done
install -Dm644 netplay/flatpak/io.github.mcerina.Coinmates.metainfo.xml $a/usr/share/metainfo/io.github.mcerina.Coinmates.appdata.xml
cp netplay/flatpak/io.github.mcerina.Coinmates.desktop $a/
rsvg-convert -w 256 -h 256 netplay/flatpak/io.github.mcerina.Coinmates.svg -o $a/io.github.mcerina.Coinmates.png
ln -s io.github.mcerina.Coinmates.png $a/.DirIcon
ln -s usr/bin/launch $a/AppRun
mkdir $a/usr/lib
cp -L /usr/lib64/{libSDL3.so.0,libSDL3_ttf.so.0,libXinerama.so.1,libXi.so.6} $a/usr/lib/
patchelf --set-rpath '$ORIGIN/../lib' $a/usr/bin/coinmates
# what the program and these libraries need, apart from each other, must be on every desktop
for f in $a/usr/bin/coinmates $a/usr/lib/*; do readelf -d $f; done | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u |
  grep -vxE 'lib(SDL3|SDL3_ttf|Xinerama|Xi)\.so\.[0-9]+' > /tmp/needed
cat /tmp/needed
if grep -vxE 'lib(c|m|stdc\+\+|gcc_s|X11|Xext|GL|fontconfig|freetype|asound)\.so\.[0-9]+' /tmp/needed; then
  echo "not on every desktop"; exit 1
fi
# appimagetool and the runtime it puts in front: their releases, the runtime without FUSE 2
get https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
mv /tmp/dl /tmp/runtime
get https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
chmod +x /tmp/dl
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 /tmp/dl --runtime-file /tmp/runtime $a coinmates-x86_64.AppImage
