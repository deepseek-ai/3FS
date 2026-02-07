#!/bin/bash

#################################################
# 3FS Dependencies Installation Script
# Supports: Ubuntu, Debian, Fedora, openSUSE,
#           openEuler, OpenCloudOS, TencentOS
#################################################

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Print functions
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Detect OS
detect_os() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        OS_LIKE=$ID_LIKE
        print_info "Detected OS: $NAME $VERSION_ID"
    else
        print_error "Cannot detect OS. /etc/os-release not found."
        exit 1
    fi
}

# Detect package manager
detect_package_manager() {
    if command -v apt &> /dev/null; then
        PKG_MANAGER="apt"
    elif command -v dnf &> /dev/null; then
        PKG_MANAGER="dnf"
    elif command -v yum &> /dev/null; then
        PKG_MANAGER="yum"
    elif command -v zypper &> /dev/null; then
        PKG_MANAGER="zypper"
    else
        print_error "No supported package manager found (apt, dnf, yum, zypper)"
        exit 1
    fi
    print_info "Package manager: $PKG_MANAGER"
}

# Check if running as root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        print_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

# Install dependencies for Ubuntu 20.04
install_ubuntu_20_04() {
    print_info "Installing dependencies for Ubuntu 20.04..."
    
    apt update
    apt install -y \
        cmake \
        libuv1-dev \
        liblz4-dev \
        liblzma-dev \
        libdouble-conversion-dev \
        libdwarf-dev \
        libunwind-dev \
        libaio-dev \
        libgflags-dev \
        libgoogle-glog-dev \
        libgtest-dev \
        libgmock-dev \
        clang-format-14 \
        clang-14 \
        clang-tidy-14 \
        lld-14 \
        libgoogle-perftools-dev \
        google-perftools \
        libssl-dev \
        libclang-rt-14-dev \
        gcc-10 \
        g++-10 \
        libboost1.71-all-dev \
        build-essential \
        git \
        wget \
        autoconf
    
    print_info "Ubuntu 20.04 dependencies installed successfully!"
}

# Install dependencies for Ubuntu 22.04
install_ubuntu_22_04() {
    print_info "Installing dependencies for Ubuntu 22.04..."
    
    apt update
    apt install -y \
        cmake \
        libuv1-dev \
        liblz4-dev \
        liblzma-dev \
        libdouble-conversion-dev \
        libdwarf-dev \
        libunwind-dev \
        libaio-dev \
        libgflags-dev \
        libgoogle-glog-dev \
        libgtest-dev \
        libgmock-dev \
        clang-format-14 \
        clang-14 \
        clang-tidy-14 \
        lld-14 \
        libgoogle-perftools-dev \
        google-perftools \
        libssl-dev \
        gcc-12 \
        g++-12 \
        libboost-all-dev \
        build-essential \
        git \
        wget \
        autoconf
    
    print_info "Ubuntu 22.04 dependencies installed successfully!"
}

# Install dependencies for Debian 11/12
install_debian() {
    print_info "Installing dependencies for Debian $VER..."
    
    apt update
    
    # Determine available clang version
    if apt-cache show clang-14 &> /dev/null; then
        CLANG_VER="14"
    elif apt-cache show clang-15 &> /dev/null; then
        CLANG_VER="15"
    elif apt-cache show clang-16 &> /dev/null; then
        CLANG_VER="16"
    else
        CLANG_VER=""
        print_warning "clang-14/15/16 not found, will install default clang"
    fi
    
    # Determine available GCC version
    if apt-cache show gcc-12 &> /dev/null; then
        GCC_VER="12"
    elif apt-cache show gcc-11 &> /dev/null; then
        GCC_VER="11"
    elif apt-cache show gcc-10 &> /dev/null; then
        GCC_VER="10"
    else
        GCC_VER=""
        print_warning "gcc-10/11/12 not found, will install default gcc"
    fi
    
    # Build package list
    PACKAGES=(
        cmake
        libuv1-dev
        liblz4-dev
        liblzma-dev
        libdouble-conversion-dev
        libdwarf-dev
        libunwind-dev
        libaio-dev
        libgflags-dev
        libgoogle-glog-dev
        libgtest-dev
        libgmock-dev
        libgoogle-perftools-dev
        google-perftools
        libssl-dev
        libboost-all-dev
        build-essential
        git
        wget
        autoconf
        pkg-config
        ninja-build
        meson
    )
    
    if [ -n "$CLANG_VER" ]; then
        PACKAGES+=(clang-$CLANG_VER clang-format-$CLANG_VER clang-tidy-$CLANG_VER lld-$CLANG_VER)
    else
        PACKAGES+=(clang clang-format clang-tidy lld)
    fi
    
    if [ -n "$GCC_VER" ]; then
        PACKAGES+=(gcc-$GCC_VER g++-$GCC_VER)
    else
        PACKAGES+=(gcc g++)
    fi
    
    apt install -y "${PACKAGES[@]}"
    
    print_info "Debian dependencies installed successfully!"
}

# Install dependencies for openEuler 2403sp1
install_openeuler() {
    print_info "Installing dependencies for openEuler 2403sp1..."
    
    yum install -y \
        cmake \
        libuv-devel \
        lz4-devel \
        xz-devel \
        double-conversion-devel \
        libdwarf-devel \
        libunwind-devel \
        libaio-devel \
        gflags-devel \
        glog-devel \
        gtest-devel \
        gmock-devel \
        clang-tools-extra \
        clang \
        lld \
        gperftools-devel \
        gperftools \
        openssl-devel \
        gcc \
        gcc-c++ \
        boost-devel \
        git \
        wget \
        autoconf
    
    print_info "openEuler dependencies installed successfully!"
}

# Install dependencies for Fedora
install_fedora() {
    print_info "Installing dependencies for Fedora $VER..."
    
    dnf install -y \
        cmake \
        libuv-devel \
        lz4-devel \
        xz-devel \
        double-conversion-devel \
        libdwarf-devel \
        libunwind-devel \
        libaio-devel \
        gflags-devel \
        glog-devel \
        gtest-devel \
        gmock-devel \
        clang \
        clang-tools-extra \
        lld \
        compiler-rt \
        gperftools-devel \
        gperftools-libs \
        openssl-devel \
        gcc \
        gcc-c++ \
        boost-devel \
        boost-static \
        git \
        wget \
        autoconf \
        automake \
        libtool \
        pkg-config \
        ninja-build \
        meson \
        perl
    
    print_info "Fedora dependencies installed successfully!"
}

# Install dependencies for openSUSE
install_opensuse() {
    print_info "Installing dependencies for openSUSE..."
    
    zypper refresh
    zypper install -y \
        cmake \
        libuv-devel \
        liblz4-devel \
        xz-devel \
        libdouble-conversion-devel \
        libdwarf-devel \
        libunwind-devel \
        libaio-devel \
        gflags-devel \
        glog-devel \
        gtest \
        gmock \
        clang \
        clang-tools \
        lld \
        gperftools-devel \
        gperftools \
        libopenssl-devel \
        gcc \
        gcc-c++ \
        boost-devel \
        libboost_headers-devel \
        libboost_filesystem-devel \
        libboost_system-devel \
        libboost_thread-devel \
        git \
        wget \
        autoconf \
        automake \
        libtool \
        pkg-config \
        ninja \
        meson \
        perl
    
    print_info "openSUSE dependencies installed successfully!"
}

# Install dependencies for OpenCloudOS 9 / TencentOS 4
install_opencloudos_tencentos() {
    print_info "Installing dependencies for OpenCloudOS 9 / TencentOS 4..."
    
    # Install epol-release if not already installed
    if ! rpm -q epol-release &> /dev/null; then
        dnf install -y epol-release
    fi
    
    dnf install -y \
        wget \
        git \
        meson \
        cmake \
        perl \
        lld \
        gcc \
        gcc-c++ \
        autoconf \
        lz4 \
        lz4-devel \
        xz \
        xz-devel \
        double-conversion-devel \
        libdwarf-devel \
        libunwind-devel \
        libaio-devel \
        gflags-devel \
        glog-devel \
        libuv-devel \
        gmock-devel \
        gperftools \
        gperftools-devel \
        openssl-devel \
        boost-static \
        boost-devel \
        mono-devel \
        libevent-devel \
        libibverbs-devel \
        numactl-devel \
        python3-devel
    
    print_info "OpenCloudOS/TencentOS dependencies installed successfully!"
}

# Install libfuse 3.16.1
install_libfuse() {
    print_info "Checking libfuse version..."
    
    FUSE_VERSION=$(pkg-config --modversion fuse3 2>/dev/null || echo "0.0.0")
    REQUIRED_VERSION="3.16.1"
    
    if [ "$(printf '%s\n' "$REQUIRED_VERSION" "$FUSE_VERSION" | sort -V | head -n1)" = "$REQUIRED_VERSION" ] && [ "$FUSE_VERSION" != "0.0.0" ]; then
        print_info "libfuse $FUSE_VERSION is already installed (>= $REQUIRED_VERSION)"
        return 0
    fi
    
    print_warning "libfuse $REQUIRED_VERSION or newer is required. Current version: $FUSE_VERSION"
    print_info "Installing libfuse 3.16.1 from source..."
    
    FUSE_DIR="/tmp/libfuse-3.16.1"
    
    if [ -d "$FUSE_DIR" ]; then
        rm -rf "$FUSE_DIR"
    fi
    
    cd /tmp
    wget https://github.com/libfuse/libfuse/releases/download/fuse-3.16.1/fuse-3.16.1.tar.gz
    tar -xzf fuse-3.16.1.tar.gz
    cd fuse-3.16.1
    
    mkdir -p build
    cd build
    meson setup .. --prefix=/usr/local
    ninja
    ninja install
    
    # Update library cache
    ldconfig
    
    cd /tmp
    rm -rf fuse-3.16.1 fuse-3.16.1.tar.gz
    
    print_info "libfuse 3.16.1 installed successfully!"
}

# Install FoundationDB 7.1
install_foundationdb() {
    print_info "Checking FoundationDB installation..."
    
    if command -v fdbcli &> /dev/null; then
        FDB_VERSION=$(fdbcli --version 2>&1 | grep -oP 'FoundationDB CLI \K[0-9.]+' | head -n1)
        print_info "FoundationDB $FDB_VERSION is already installed"
        
        # Check if version is >= 7.1
        if [ "$(printf '%s\n' "7.1" "$FDB_VERSION" | sort -V | head -n1)" = "7.1" ]; then
            return 0
        fi
    fi
    
    print_warning "FoundationDB 7.1 or newer is required."
    print_info "Installing FoundationDB 7.1..."
    
    # Detect architecture
    ARCH=$(uname -m)
    if [ "$ARCH" = "x86_64" ]; then
        FDB_ARCH="amd64"
    elif [ "$ARCH" = "aarch64" ]; then
        FDB_ARCH="arm64"
    else
        print_error "Unsupported architecture: $ARCH"
        return 1
    fi
    
    cd /tmp
    
    # Download appropriate packages based on OS
    if [[ "$OS" == "ubuntu" ]] || [[ "$OS" == "debian" ]]; then
        wget https://github.com/apple/foundationdb/releases/download/7.1.61/foundationdb-clients_7.1.61-1_${FDB_ARCH}.deb
        wget https://github.com/apple/foundationdb/releases/download/7.1.61/foundationdb-server_7.1.61-1_${FDB_ARCH}.deb
        
        dpkg -i foundationdb-clients_7.1.61-1_${FDB_ARCH}.deb
        dpkg -i foundationdb-server_7.1.61-1_${FDB_ARCH}.deb
        
        rm -f foundationdb-*.deb
    else
        # For RPM-based systems
        if [ "$FDB_ARCH" = "amd64" ]; then
            FDB_ARCH="x86_64"
        fi
        
        wget https://github.com/apple/foundationdb/releases/download/7.1.61/foundationdb-clients-7.1.61-1.el7.${FDB_ARCH}.rpm
        wget https://github.com/apple/foundationdb/releases/download/7.1.61/foundationdb-server-7.1.61-1.el7.${FDB_ARCH}.rpm
        
        rpm -Uvh foundationdb-clients-7.1.61-1.el7.${FDB_ARCH}.rpm
        rpm -Uvh foundationdb-server-7.1.61-1.el7.${FDB_ARCH}.rpm
        
        rm -f foundationdb-*.rpm
    fi
    
    print_info "FoundationDB installed successfully!"
}

# Install Rust toolchain
install_rust() {
    print_info "Checking Rust installation..."
    
    if command -v rustc &> /dev/null; then
        RUST_VERSION=$(rustc --version | awk '{print $2}')
        print_info "Rust $RUST_VERSION is already installed"
        
        # Check if version is >= 1.75.0
        if [ "$(printf '%s\n' "1.75.0" "$RUST_VERSION" | sort -V | head -n1)" = "1.75.0" ]; then
            print_info "Rust version meets the minimum requirement (>= 1.75.0)"
            
            # Update to latest stable if current version is old
            if [ "$(printf '%s\n' "1.85.0" "$RUST_VERSION" | sort -V | head -n1)" != "1.85.0" ]; then
                print_warning "Updating Rust to the latest stable version (recommended 1.85.0+)..."
                rustup update stable
            fi
            return 0
        fi
    fi
    
    print_info "Installing Rust toolchain..."
    
    # Install Rust using rustup
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
    
    # Source cargo environment
    source "$HOME/.cargo/env" || source /root/.cargo/env
    
    RUST_VERSION=$(rustc --version | awk '{print $2}')
    print_info "Rust $RUST_VERSION installed successfully!"
}

# Main installation process
main() {
    print_info "Starting 3FS dependencies installation..."
    
    check_root
    detect_os
    detect_package_manager
    
    # Install OS-specific dependencies
    case "$OS" in
        ubuntu)
            if [[ "$VER" == "20.04" ]]; then
                install_ubuntu_20_04
            elif [[ "$VER" == "22.04" ]]; then
                install_ubuntu_22_04
            elif [[ "$VER" == "24.04" ]]; then
                install_ubuntu_22_04  # Same as 22.04
            else
                print_warning "Unsupported Ubuntu version: $VER, trying Ubuntu 22.04 packages..."
                install_ubuntu_22_04
            fi
            ;;
        debian)
            install_debian
            ;;
        fedora)
            install_fedora
            ;;
        opensuse|opensuse-leap|opensuse-tumbleweed|sles)
            install_opensuse
            ;;
        openeuler|openEuler)
            install_openeuler
            ;;
        opencloudos|tencentos)
            install_opencloudos_tencentos
            ;;
        rhel|centos|rocky|almalinux)
            # RHEL-based distributions
            if [[ "$VER" =~ ^9 ]] || [[ "$VER" =~ ^4 ]]; then
                install_opencloudos_tencentos
            else
                print_warning "Unsupported $NAME version: $VER"
                print_info "Trying OpenCloudOS/TencentOS installation method..."
                install_opencloudos_tencentos
            fi
            ;;
        *)
            # Try to detect based on package manager and ID_LIKE
            print_warning "OS '$OS' not explicitly supported, attempting installation based on package manager..."
            
            if [[ "$OS_LIKE" == *"debian"* ]] && [ "$PKG_MANAGER" = "apt" ]; then
                print_info "Detected Debian-like system, using Debian installation..."
                install_debian
            elif [[ "$OS_LIKE" == *"rhel"* ]] || [[ "$OS_LIKE" == *"fedora"* ]]; then
                if [ "$PKG_MANAGER" = "dnf" ]; then
                    print_info "Detected Fedora-like system, using Fedora installation..."
                    install_fedora
                elif [ "$PKG_MANAGER" = "yum" ]; then
                    print_info "Detected RHEL-like system, using OpenCloudOS/TencentOS installation..."
                    install_opencloudos_tencentos
                fi
            elif [[ "$OS_LIKE" == *"suse"* ]] && [ "$PKG_MANAGER" = "zypper" ]; then
                print_info "Detected SUSE-like system, using openSUSE installation..."
                install_opensuse
            else
                print_error "Unsupported OS: $OS"
                print_info "Supported OS: Ubuntu, Debian, Fedora, openSUSE, openEuler, OpenCloudOS, TencentOS, RHEL-based"
                exit 1
            fi
            ;;
    esac
    
    # Install common prerequisites
    print_info "Installing additional prerequisites..."
    install_libfuse
    install_foundationdb
    
    # Note: Rust installation doesn't require root, so we show instructions instead
    print_warning "Rust installation should be done by the user (non-root):"
    print_info "Run: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    print_info "Or if root, the script will attempt to install for root user..."
    install_rust || print_warning "Rust installation failed. Please install manually."
    
    print_info ""
    print_info "============================================"
    print_info "${GREEN}All dependencies installed successfully!${NC}"
    print_info "============================================"
    print_info ""
    print_info "Next steps:"
    print_info "1. Clone the repository: git clone https://github.com/deepseek-ai/3fs"
    print_info "2. Initialize submodules: cd 3fs && git submodule update --init --recursive"
    print_info "3. Apply patches: ./patches/apply.sh"
    print_info "4. Build: cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo"
    print_info "5. Compile: cmake --build build -j \$(nproc)"
    print_info ""
    print_info "Note: Adjust compiler versions based on what was installed (e.g., clang++-14, clang-14)"
    print_info ""
}

# Run main function
main "$@"

