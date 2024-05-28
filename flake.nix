{
  inputs = {
    systems.url = "github:nix-systems/default";
  };

  outputs = {
    systems,
    nixpkgs,
    ...
  }: let
    forAllSystems = nixpkgs.lib.genAttrs (import systems);
  in {
    devShells = forAllSystems (system: let 
      pkgs = nixpkgs.legacyPackages.${system};
      legacy = import (builtins.fetchTarball {
        url = "https://github.com/NixOS/nixpkgs/archive/9957cd48326fe8dbd52fdc50dd2502307f188b0d.tar.gz";
        sha256 = "sha256:1l2hq1n1jl2l64fdcpq3jrfphaz10sd1cpsax3xdya0xgsncgcsi";
      }) { inherit system; config.permittedInsecurePackages = [ "nodejs-14.21.3" "openssl-1.1.1w" ]; };
    in {
      default = pkgs.mkShell {
        buildInputs = [
          legacy.nodejs_14
          pkgs.dart-sass
        ];
      };
    });
  };
}
