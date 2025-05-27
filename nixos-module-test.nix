# NixOS VM test for 3FS services
# Run with: nix build .#checks.x86_64-linux.3fs-integration-test

{ pkgs, lib, self, ... }:

let
  # Test configuration for 3FS cluster
  testConfig = {
    meta = {
      port = 6000;
      dataDir = "/var/lib/3fs/meta";
    };
    
    storage = {
      port = 7000;
      targets = [
        "/var/lib/3fs/storage/target1"
        "/var/lib/3fs/storage/target2"
      ];
    };
    
    mgmtd = {
      port = 8000;
      dataDir = "/var/lib/3fs/mgmtd";
    };
  };
  
in {
  name = "3fs-integration-test";
  
  nodes = {
    # Master node running all services
    master = { config, pkgs, ... }: {
      imports = [ self.nixosModules.default ];
      
      # Enable 3FS services
      services."3fs" = {
        enable = true;
        
        meta = {
          enable = true;
          config = {
            inherit (testConfig.meta) port dataDir;
          };
        };
        
        storage = {
          enable = true;
          targets = testConfig.storage.targets;
          config = {
            inherit (testConfig.storage) port;
          };
        };
        
        mgmtd = {
          enable = true;
          config = {
            inherit (testConfig.mgmtd) port dataDir;
          };
        };
        
        monitor = {
          enable = true;
          config = {
            port = 9000;
          };
        };
        
        # FoundationDB configuration is handled by the system service
      };
      
      # Configure FoundationDB
      services.foundationdb = {
        enable = true;
        package = pkgs.foundationdb;
        listenAddress = "127.0.0.1:4500";
        dataDir = "/var/lib/foundationdb";
        logDir = "/var/log/foundationdb";
      };
      
      # Open firewall ports
      networking.firewall.enable = false;
      
      # Additional test utilities
      environment.systemPackages = with pkgs; [
        netcat
        curl
        jq
      ];
    };
    
    # Client node with FUSE mount
    client = { config, pkgs, ... }: {
      imports = [ self.nixosModules.default ];
      
      services."3fs" = {
        enable = true;
        
        fuse = {
          enable = true;
          mountPoint = "/mnt/3fs";
          config = {
            metaServers = [ "master:6000" ];
            mgmtdServers = [ "master:8000" ];
          };
        };
        
        # FoundationDB configuration is handled by the system service
      };
      
      # Configure FoundationDB client
      services.foundationdb = {
        enable = true;
        package = pkgs.foundationdb;
      };
      
      # Write cluster file pointing to master
      environment.etc."foundationdb/fdb.cluster" = {
        text = "test:test@master:4500";
        mode = "0644";
      };
      
      networking.firewall.enable = false;
      
      environment.systemPackages = with pkgs; [
        fio
        sysbench
      ];
    };
  };
  
  testScript = ''
    start_all()
    
    # Wait for FoundationDB to be ready
    master.wait_for_unit("foundationdb.service")
    master.wait_for_open_port(4500)
    master.succeed("fdbcli --exec 'status' -C /etc/foundationdb/fdb.cluster")
    
    # Wait for 3FS services to start
    master.wait_for_unit("3fs-mgmtd.service")
    master.wait_for_unit("3fs-meta.service")
    master.wait_for_unit("3fs-storage.service")
    master.wait_for_unit("3fs-monitor.service")
    
    # Check that services are listening on correct ports
    master.wait_for_open_port(${toString testConfig.meta.port})
    master.wait_for_open_port(${toString testConfig.storage.port})
    master.wait_for_open_port(${toString testConfig.mgmtd.port})
    master.wait_for_open_port(9000)  # monitor
    
    # Check service status
    master.succeed("systemctl is-active 3fs-mgmtd.service")
    master.succeed("systemctl is-active 3fs-meta.service")
    master.succeed("systemctl is-active 3fs-storage.service")
    master.succeed("systemctl is-active 3fs-monitor.service")
    
    # Initialize 3FS cluster
    master.succeed("${pkgs."3fs"}/bin/admin init-cluster --config /etc/foundationdb/fdb.cluster")
    
    # Register storage nodes
    master.succeed("${pkgs."3fs"}/bin/admin register-node --type storage --address master:${toString testConfig.storage.port}")
    
    # Create storage targets
    for target in ${lib.concatStringsSep " " (map (t: "\"${t}\"") testConfig.storage.targets)}; do
      master.succeed("${pkgs."3fs"}/bin/admin create-target --path $target")
    done
    
    # Wait for client to connect
    client.wait_for_unit("3fs-fuse.service")
    client.wait_until_succeeds("mountpoint -q /mnt/3fs")
    
    # Basic filesystem operations test
    client.succeed("echo 'Hello 3FS!' > /mnt/3fs/test.txt")
    client.succeed("cat /mnt/3fs/test.txt | grep 'Hello 3FS!'")
    
    # Create directory structure
    client.succeed("mkdir -p /mnt/3fs/test/deep/directory")
    client.succeed("touch /mnt/3fs/test/deep/directory/file.txt")
    client.succeed("ls -la /mnt/3fs/test/deep/directory/")
    
    # Test file operations
    client.succeed("dd if=/dev/urandom of=/mnt/3fs/random.dat bs=1M count=10")
    client.succeed("cp /mnt/3fs/random.dat /mnt/3fs/random_copy.dat")
    client.succeed("cmp /mnt/3fs/random.dat /mnt/3fs/random_copy.dat")
    
    # Test permissions
    client.succeed("chmod 755 /mnt/3fs/test")
    client.succeed("test -d /mnt/3fs/test")
    
    # Clean up
    client.succeed("rm -rf /mnt/3fs/test*")
    client.succeed("rm -f /mnt/3fs/random*.dat")
    
    # Verify cleanup
    client.succeed("test -z \"$(ls -A /mnt/3fs)\"")
    
    # Check metrics
    master.succeed("${pkgs."3fs"}/bin/admin list-nodes")
    master.succeed("${pkgs."3fs"}/bin/admin list-targets")
    
    # Test service restart
    master.succeed("systemctl restart 3fs-storage.service")
    master.wait_for_unit("3fs-storage.service")
    
    # Ensure filesystem still works after restart
    client.succeed("echo 'Still working!' > /mnt/3fs/restart-test.txt")
    client.succeed("cat /mnt/3fs/restart-test.txt")
  '';
}