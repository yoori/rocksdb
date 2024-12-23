%bcond_without compression

Name:    rocksdb
Version: %{_version}
Release: 2%{?dist}
Summary: A Persistent Key-Value Store for Flash and RAM Storage (with coroutines support)

License: GPLv2 or ASL 2.0 and BSD
URL:     https://github.com/yoori/rocksdb.git
Source0: https://github.com/yoori/rocksdb/archive/refs/tags/v%{_version}.tar.gz

Requires: liburing >= 2.0.0
Requires: gflags >= 2.0.0

%if %{with compression}
Requires: bzip2-devel
Requires: lz4-devel
Requires: snappy-devel
Requires: zlib-devel
Requires: libzstd-devel
%endif

BuildRequires: gcc-toolset-10-gcc-c++
BuildRequires: cmake
BuildRequires: gflags-devel >= 2.0.0
BuildRequires: liburing-devel >= 2.0.0

%if %{with compression}
BuildRequires: bzip2-devel
BuildRequires: lz4-devel
BuildRequires: snappy-devel
BuildRequires: zlib-devel
BuildRequires: libzstd-devel
%endif

BuildRequires: /usr/bin/perl
BuildRequires: python3-devel

BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{_version}-%{release}-XXXXXX)

%description
RocksDB is a library that forms the core building block for a fast key value
server, especially suited for storing data on flash drives. It has a
Log-Structured-Merge-Database (LSM) design with flexible trade offs between
Write-Amplification-Factor (WAF), Read-Amplification-Factor (RAF) and
Space-Amplification-Factor (SAF). It has multi-threaded compaction, making it
specially suitable for storing multiple terabytes of data in a single database.

%package tools
Summary: Utility tools for RocksDB
Requires: %{name}%{?_isa} = %{version}-%{release}

%description tools
Utility tools for RocksDB.

%package devel
Summary: Development files for RocksDB
Requires: %{name}%{?_isa} = %{version}-%{release}

%description devel
Development files for RocksDB.


%prep
%setup -q -n rocksdb-%{_version}

%build

%cmake \
%if %{with compression}
  -DWITH_BZ2=ON \
  -DWITH_SNAPPY=ON \
  -DWITH_LZ4=ON \
  -DWITH_ZSTD=ON \
  -DWITH_ZLIB=ON \
%endif
  -GNinja \
  -DROCKSDB_BUILD_SHARED=ON \
  -DWITH_BENCHMARK_TOOLS=ON \
  -DWITH_CORE_TOOLS=ON \
  -DWITH_TOOLS=ON \
  -DUSE_RTTI=ON \
  -DPORTABLE=1 \
  -DFAIL_ON_WARNINGS=OFF \
  -DWITH_TESTS=ON \
  -DCMAKE_SKIP_BUILD_RPATH=ON \
  -DCMAKE_CXX_COMPILER='/opt/rh/gcc-toolset-10/root/usr/bin/g++' \
  -DCMAKE_C_COMPILER='/opt/rh/gcc-toolset-10/root/usr/bin/gcc' \
  -DCMAKE_CXX_STANDARD=20 \
  -DBENCHMARK_ENABLE_GTEST_TESTS=0

%cmake_build


%install
%cmake_install

# Missing steps in build script
install -dD -m 755 %{buildroot}%{_bindir}
install -m 755 %{__cmake_builddir}/cache_bench %{buildroot}%{_bindir}/cache_bench
install -m 755 %{__cmake_builddir}/db_bench %{buildroot}%{_bindir}/db_bench
install -m 755 %{__cmake_builddir}/tools/ldb %{buildroot}%{_bindir}/ldb
install -m 755 %{__cmake_builddir}/tools/sst_dump %{buildroot}%{_bindir}/sst_dump


%files
%doc README.md
%doc HISTORY.md
%doc AUTHORS
%license COPYING
%license LICENSE.Apache
%license LICENSE.leveldb
%{_libdir}/librocksdb.so.*


%files tools
%doc README.md
%license COPYING
%license LICENSE.Apache
%license LICENSE.leveldb
%{_bindir}/cache_bench
%{_bindir}/db_bench
%{_bindir}/ldb
%{_bindir}/sst_dump


%files devel
%doc README.md
%doc LANGUAGE-BINDINGS.md
%license COPYING
%license LICENSE.Apache
%license LICENSE.leveldb
%{_libdir}/librocksdb.so
%{_libdir}/librocksdb.a
%{_libdir}/cmake/rocksdb
# {_libdir}/pkgconfig/rocksdb.pc
%{_includedir}/rocksdb


%changelog
