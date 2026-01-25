{
  description = "NAR stream patcher for Android compatibility";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:

    let
      inherit (nixpkgs) lib;

      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = lib.genAttrs supportedSystems;

      version = lib.removeSuffix "\n" (builtins.readFile ./version);

      srcFiles = [
        ./COPYING
        ./README.md
        ./Makefile.am
        ./configure.ac
        ./m4
        ./src
        ./tests
        ./version
      ];

      src = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions srcFiles;
      };

      patchnarFor = pkgs: pkgs.callPackage ./package.nix {
        inherit version src;
      };

    in
    {
      overlays.default = final: prev: {
        patchnar = patchnarFor final;
      };

      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          patchnar = patchnarFor pkgs;
          default = self.packages.${system}.patchnar;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = self.packages.${system}.patchnar.overrideAttrs (old: {
            nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [
              pkgs.gdb
            ];
          });
        }
      );
    };
}
