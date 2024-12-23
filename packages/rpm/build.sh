#!/bin/bash

VERSION=$1
VERSIONSSV=ssv1

# script require 'sudo rpm' for install RPM packages
# and access to mirror.yandex.ru repository

# create build/RPMS folder - all built packages will be duplicated here
SCRIPTPATH="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
RES_TMP=build/TMP/
RES_RPMS=build/RPMS/
rm -rf "$RES_TMP"

mkdir -p $RES_TMP
mkdir -p $RES_RPMS

# download and install packages required for build
sudo yum -y install spectool yum-utils rpmdevtools redhat-rpm-config rpm-build \
  epel-rpm-macros \
  || \
  { echo "can't install base packages" >&2 ; exit 1 ; }

# create folders for RPM build environment
mkdir -vp  `rpm -E '%_tmppath %_rpmdir %_builddir %_sourcedir %_specdir %_srcrpmdir %_rpmdir/%_arch'`

BIN_RPM_FOLDER=$(rpm -E '%_rpmdir/%_arch')

SPEC_FILE=$(rpm -E %_specdir)/rocksdb-$VERSION-$VERSIONSSV.spec

# download spec file
cp $SCRIPTPATH/rocksdb.spec "$SPEC_FILE"

echo ">>>> To install requirements."

$SUDO_PREFIX yum-builddep -y "$SPEC_FILE" || \
  { echo "can't install build requirements" >&2 ; exit 1 ; }

echo ">>>> To download sources."

spectool --force -g -R --define "_version $VERSION" "$SPEC_FILE" || \
  { echo "can't download sources" >&2 ; exit 1 ; }

pushd /opt/rh/gcc-toolset-10/root/usr/lib/gcc/x86_64-redhat-linux/10/plugin/
ln -s annobin.so gcc-annobin.so 2>/dev/null
popd

rpmbuild --force -ba --define "_version $VERSION" "$SPEC_FILE" || \
  { echo "can't build RPM" >&2 ; exit 1 ; }

