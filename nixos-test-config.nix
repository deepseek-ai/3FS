# Example NixOS configuration for testing 3FS
{ config, pkgs, ... }:

{
  imports = [
    # Import the 3FS flake module
    # In a real system, you would use:
    # (builtins.getFlake "github:deepseek-ai/3fs").nixosModules.default
  ];

  # Enable 3FS services
  services."3fs" = {
    enable = true;
    
    # Configure user and group
    user = "threefs";
    group = "threefs";
    
    # Configure directories
    configDir = "/etc/3fs";
    dataDir = "/var/lib/3fs";
    
    # Enable metadata service
    meta = {
      enable = true;
      config = {
        # Additional meta service configuration
      };
    };
    
    # Enable storage service
    storage = {
      enable = true;
      targets = [
        "/var/lib/3fs/storage/target1"
        "/var/lib/3fs/storage/target2"
      ];
      config = {
        # Additional storage service configuration
      };
    };
    
    # Enable management daemon
    mgmtd = {
      enable = true;
      config = {
        # Additional mgmtd configuration
      };
    };
    
    # Enable monitor collector
    monitor = {
      enable = true;
      config = {
        # Additional monitor configuration
      };
    };
    
    # Enable FUSE client
    fuse = {
      enable = true;
      mountPoint = "/mnt/3fs";
      config = {
        # Additional FUSE configuration
      };
    };
    
    # FoundationDB configuration
    foundationdb = {
      clusterFile = "/etc/foundationdb/fdb.cluster";
    };
  };
  
  # Ensure FoundationDB is also installed and configured
  services.foundationdb = {
    enable = true;
    clusterFile = "/etc/foundationdb/fdb.cluster";
  };
  
  # Open necessary ports for 3FS services
  networking.firewall = {
    allowedTCPPorts = [
      # Meta service ports
      6000 6001
      # Storage service ports  
      7000 7001
      # Management daemon ports
      8000 8001
      # Monitor collector ports
      9000 9001
    ];
  };
  
  # Example of how to override package
  nixpkgs.overlays = [
    (final: prev: {
      "3fs" = prev."3fs".overrideAttrs (oldAttrs: {
        # Custom overrides if needed
      });
    })
  ];
}