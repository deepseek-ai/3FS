{
  description = "Fire-Flyer File System (3FS) - High-performance distributed file system for AI workloads";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      overlay = final: prev: {
        "3fs" = prev.callPackage ./package.nix { };
      };
      
      nixosModule = { config, lib, pkgs, ... }:
        with lib;
        let
          cfg = config.services."3fs";
          
          # Helper function to create service configuration
          mkServiceConfig = component: {
            description = "3FS ${component} service";
            wantedBy = [ "multi-user.target" ];
            after = [ "network-online.target" ];
            requires = [ "network-online.target" ];
            
            serviceConfig = {
              Type = "simple";
              ExecStart = "${pkgs."3fs"}/bin/${component}_main --launcher_cfg ${cfg.configDir}/${component}_main_launcher.toml --app-cfg ${cfg.configDir}/${component}_main_app.toml";
              Restart = "on-failure";
              RestartSec = 5;
              LimitNOFILE = 1000000;
              User = cfg.user;
              Group = cfg.group;
            } // (if component == "storage" then {
              LimitMEMLOCK = "infinity";
              TimeoutStopSec = "5m";
            } else {});
            
            environment = {
              LD_LIBRARY_PATH = "${pkgs."3fs"}/lib:${pkgs.foundationdb}/lib";
            };
          };
        in
        {
          options.services."3fs" = {
            enable = mkEnableOption "3FS distributed file system";
            
            user = mkOption {
              type = types.str;
              default = "threefs";
              description = "User under which 3FS services run";
            };
            
            group = mkOption {
              type = types.str;
              default = "threefs";
              description = "Group under which 3FS services run";
            };
            
            configDir = mkOption {
              type = types.path;
              default = "/etc/3fs";
              description = "Directory containing 3FS configuration files";
            };
            
            dataDir = mkOption {
              type = types.path;
              default = "/var/lib/3fs";
              description = "Directory for 3FS data storage";
            };
            
            meta = {
              enable = mkEnableOption "3FS metadata service";
              
              config = mkOption {
                type = types.attrs;
                default = {};
                description = "Additional configuration for meta service";
              };
            };
            
            storage = {
              enable = mkEnableOption "3FS storage service";
              
              targets = mkOption {
                type = types.listOf types.str;
                default = [ "/var/lib/3fs/storage" ];
                description = "List of storage target directories";
              };
              
              config = mkOption {
                type = types.attrs;
                default = {};
                description = "Additional configuration for storage service";
              };
            };
            
            mgmtd = {
              enable = mkEnableOption "3FS management daemon";
              
              config = mkOption {
                type = types.attrs;
                default = {};
                description = "Additional configuration for management daemon";
              };
            };
            
            monitor = {
              enable = mkEnableOption "3FS monitor collector";
              
              config = mkOption {
                type = types.attrs;
                default = {};
                description = "Additional configuration for monitor collector";
              };
            };
            
            fuse = {
              enable = mkEnableOption "3FS FUSE client";
              
              mountPoint = mkOption {
                type = types.path;
                default = "/mnt/3fs";
                description = "Mount point for 3FS FUSE filesystem";
              };
              
              config = mkOption {
                type = types.attrs;
                default = {};
                description = "Additional configuration for FUSE client";
              };
            };
            
            foundationdb = {
              clusterFile = mkOption {
                type = types.path;
                default = "/etc/foundationdb/fdb.cluster";
                description = "Path to FoundationDB cluster file";
              };
            };
          };
          
          config = mkIf cfg.enable {
            # Create system user and group
            users.users.${cfg.user} = {
              isSystemUser = true;
              group = cfg.group;
              home = cfg.dataDir;
              createHome = true;
              description = "3FS system user";
            };
            
            users.groups.${cfg.group} = {};
            
            # Create necessary directories
            systemd.tmpfiles.rules = [
              "d '${cfg.configDir}' 0755 ${cfg.user} ${cfg.group} -"
              "d '${cfg.dataDir}' 0755 ${cfg.user} ${cfg.group} -"
              "d '${cfg.dataDir}/meta' 0755 ${cfg.user} ${cfg.group} -"
              "d '${cfg.dataDir}/mgmtd' 0755 ${cfg.user} ${cfg.group} -"
              "d '${cfg.dataDir}/monitor' 0755 ${cfg.user} ${cfg.group} -"
            ] ++ (map (target: "d '${target}' 0755 ${cfg.user} ${cfg.group} -") cfg.storage.targets);
            
            # Install default configuration files
            environment.etc = {
              "3fs/meta_main_launcher.toml".source = "${pkgs."3fs"}/etc/3fs/meta_main_launcher.toml";
              "3fs/meta_main_app.toml".source = "${pkgs."3fs"}/etc/3fs/meta_main_app.toml";
              "3fs/storage_main_launcher.toml".source = "${pkgs."3fs"}/etc/3fs/storage_main_launcher.toml";
              "3fs/storage_main_app.toml".source = "${pkgs."3fs"}/etc/3fs/storage_main_app.toml";
              "3fs/mgmtd_main_launcher.toml".source = "${pkgs."3fs"}/etc/3fs/mgmtd_main_launcher.toml";
              "3fs/mgmtd_main_app.toml".source = "${pkgs."3fs"}/etc/3fs/mgmtd_main_app.toml";
              "3fs/monitor_collector_main.toml".source = "${pkgs."3fs"}/etc/3fs/monitor_collector_main.toml";
              "3fs/hf3fs_fuse_main_launcher.toml".source = "${pkgs."3fs"}/etc/3fs/hf3fs_fuse_main_launcher.toml";
              "3fs/hf3fs_fuse_main_app.toml".source = "${pkgs."3fs"}/etc/3fs/hf3fs_fuse_main_app.toml";
            };
            
            # Define systemd services
            systemd.services = {
              "3fs-meta" = mkIf cfg.meta.enable (mkServiceConfig "meta");
              "3fs-storage" = mkIf cfg.storage.enable (mkServiceConfig "storage");
              "3fs-mgmtd" = mkIf cfg.mgmtd.enable (mkServiceConfig "mgmtd");
              "3fs-monitor" = mkIf cfg.monitor.enable (mkServiceConfig "monitor_collector");
              
              "3fs-fuse" = mkIf cfg.fuse.enable {
                description = "3FS FUSE client";
                wantedBy = [ "multi-user.target" ];
                after = [ "network-online.target" "3fs-meta.service" ];
                requires = [ "network-online.target" ];
                
                serviceConfig = {
                  Type = "simple";
                  ExecStart = "${pkgs."3fs"}/bin/hf3fs_fuse_main --launcher_cfg ${cfg.configDir}/hf3fs_fuse_main_launcher.toml --app-cfg ${cfg.configDir}/hf3fs_fuse_main_app.toml ${cfg.fuse.mountPoint}";
                  ExecStop = "${pkgs.fuse3}/bin/fusermount3 -u ${cfg.fuse.mountPoint}";
                  Restart = "on-failure";
                  RestartSec = 5;
                  LimitNOFILE = 1000000;
                  User = "root";  # FUSE requires root
                  Group = "root";
                };
                
                preStart = ''
                  mkdir -p ${cfg.fuse.mountPoint}
                '';
              };
            };
            
            # Add 3fs package to system packages
            environment.systemPackages = [ pkgs."3fs" ];
          };
        };
    in
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ overlay ];
        };
      in
      {
        packages = {
          default = pkgs."3fs";
          "3fs" = pkgs."3fs";
        };

        overlays.default = overlay;

        devShells.default = pkgs.mkShell {
          inputsFrom = [ pkgs."3fs" ];
          buildInputs = with pkgs; [
            # Additional dev tools
            clang-tools_14
            cmake-format
            cmake-language-server
            gdb
            valgrind
            perf-tools
            rust-analyzer
            cargo-watch
          ];
          
          shellHook = ''
            echo "3FS development environment"
            echo "Build with: nix build .#3fs"
            echo "Enter shell with: nix develop"
            echo ""
            echo "To test locally:"
            echo "  1. Start FoundationDB"
            echo "  2. Configure and start 3FS services"
            echo "  3. Mount FUSE filesystem"
          '';
        };
        checks = {
          # Package build test
          package = pkgs."3fs";
          
          # Integration test using NixOS VM
          integration-test = pkgs.nixosTest (import ./nixos-module-test.nix {
            inherit pkgs lib;
            self = self;
          });
        };
      }
    ) // {
      nixosModules.default = nixosModule;
      nixosModules."3fs" = nixosModule;
      
      # Hydra job for CI
      hydraJobs = {
        inherit (self) packages;
        tests = self.checks;
      };
    };
}