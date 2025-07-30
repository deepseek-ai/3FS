# 3FS Nix Package and NixOS Module

This directory contains a Nix flake for building and deploying 3FS (Fire-Flyer File System) on NixOS systems.

## Features

- **Complete Package**: Builds 3FS with all dependencies including RDMA support, FoundationDB integration, and FUSE 3
- **NixOS Module**: Provides systemd services for all 3FS components (meta, storage, mgmtd, monitor, fuse)
- **Development Shell**: Includes all build tools and dependencies for 3FS development
- **Integration Tests**: NixOS VM tests to validate the complete system

## Quick Start

### Building the Package

1. First build (will fail with hash mismatch):
```bash
nix build .#3fs
```

2. Update the hash in `package.nix` with the correct one from the error message

3. Rebuild:
```bash
nix build .#3fs
```

### Using in NixOS

Add to your `flake.nix`:

```nix
{
  inputs = {
    threefs.url = "github:deepseek-ai/3fs";
  };
  
  outputs = { self, nixpkgs, threefs, ... }: {
    nixosConfigurations.myserver = nixpkgs.lib.nixosSystem {
      modules = [
        threefs.nixosModules.default
        {
          services."3fs" = {
            enable = true;
            meta.enable = true;
            storage.enable = true;
            mgmtd.enable = true;
          };
        }
      ];
    };
  };
}
```

### Development Environment

Enter the development shell:
```bash
nix develop
```

This provides:
- Clang 14 with C++20 support
- CMake and build tools
- All 3FS dependencies
- Rust toolchain
- Debug tools (gdb, valgrind, perf)

## NixOS Module Options

### Basic Configuration

```nix
services."3fs" = {
  enable = true;
  
  # User and group for services
  user = "threefs";
  group = "threefs";
  
  # Directories
  configDir = "/etc/3fs";
  dataDir = "/var/lib/3fs";
  
  # FoundationDB cluster file
  foundationdb.clusterFile = "/etc/foundationdb/fdb.cluster";
};
```

### Service Configuration

#### Metadata Service
```nix
services."3fs".meta = {
  enable = true;
  config = {
    # Additional meta configuration
  };
};
```

#### Storage Service
```nix
services."3fs".storage = {
  enable = true;
  targets = [
    "/var/lib/3fs/storage/target1"
    "/var/lib/3fs/storage/target2"
  ];
  config = {
    # Additional storage configuration
  };
};
```

#### Management Daemon
```nix
services."3fs".mgmtd = {
  enable = true;
  config = {
    # Additional mgmtd configuration
  };
};
```

#### Monitor Collector
```nix
services."3fs".monitor = {
  enable = true;
  config = {
    # Additional monitor configuration
  };
};
```

#### FUSE Client
```nix
services."3fs".fuse = {
  enable = true;
  mountPoint = "/mnt/3fs";
  config = {
    # Additional FUSE configuration
  };
};
```

## Testing

### Run Package Tests
```bash
./test-build.sh
```

### Run Integration Tests
```bash
nix build .#checks.x86_64-linux.integration-test
```

### Manual Testing

1. Start FoundationDB:
```bash
sudo systemctl start foundationdb
```

2. Initialize 3FS cluster:
```bash
3fs-admin init-cluster
```

3. Start 3FS services:
```bash
sudo systemctl start 3fs-mgmtd
sudo systemctl start 3fs-meta
sudo systemctl start 3fs-storage
```

4. Mount filesystem:
```bash
sudo systemctl start 3fs-fuse
```

## Package Contents

The package includes:

- **Binaries**:
  - `admin` - Administrative CLI tool
  - `meta_main` - Metadata service
  - `storage_main` - Storage service
  - `mgmtd_main` - Management daemon
  - `monitor_collector_main` - Metrics collector
  - `hf3fs_fuse_main` - FUSE filesystem client

- **Libraries**: All required shared libraries in `/lib`
- **Configuration**: Default configuration files in `/etc/3fs`
- **Headers**: Development headers in `/include/3fs`

## Troubleshooting

### Build Issues

1. **Hash mismatch**: Update the SHA256 hash in `package.nix`
2. **Missing dependencies**: Check that all system libraries are available
3. **Compilation errors**: Ensure you're using Clang 14 or newer

### Runtime Issues

1. **Service fails to start**: Check logs with `journalctl -u 3fs-<service>`
2. **FoundationDB connection**: Ensure FDB cluster file is correct
3. **FUSE mount fails**: Check that FUSE 3 is installed and kernel module is loaded

## Architecture

The Nix package handles:

1. **Complex Dependencies**: Automatically builds custom jemalloc, handles RDMA libraries
2. **Third-party Libraries**: Uses system libraries where possible, builds others from source
3. **Rust Components**: Properly integrates Cargo builds for Rust components
4. **Service Management**: Complete systemd integration with proper dependencies
5. **Configuration**: Manages TOML configuration files for all components

## Contributing

When modifying the package:

1. Update version in `package.nix`
2. Test build locally
3. Run integration tests
4. Update documentation

## License

MIT License (same as 3FS project)