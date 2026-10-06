# The X4 VR mod's native Linux build (README.md, section Linux). Classic Nix, no flakes:
#   nix-build linux/nix                        (from the repository root)
#   pkgs.callPackage ./linux/nix { }           (from configuration.nix)
# Built from your channel's nixpkgs by default (--arg pkgs for another): the mod is loaded into X4,
# which runs with the system's glibc, and a newer glibc than the system's would keep it from loading.
# Only what is used below is built or fetched: Nix evaluates nixpkgs lazily.
{ pkgs ? import <nixpkgs> { } }:
let
  inherit (pkgs) lib;
  root = ../..;
in
pkgs.stdenv.mkDerivation {
  pname = "x4vr";
  version = "0.2.0";

  # The source nix-build copies into the store: the whole repository (Windows parts too; the
  # Linux CMake build only compiles its own and the shared files), minus .git, editor and build
  # leftovers (cleanSourceFilter), local build trees (build*), nix-build links (result*),
  # reports/ and a manual OpenVR clone (external/). Any change to what is copied rebuilds.
  src = lib.cleanSourceWith {
    src = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in lib.cleanSourceFilter path type
         && !(lib.hasPrefix "build" rel) && !(lib.hasPrefix "result" rel)
         && !(lib.hasPrefix "reports" rel) && !(lib.hasPrefix "external" rel);
  };

  nativeBuildInputs = [ pkgs.cmake ];
  buildInputs = [ pkgs.vulkan-headers ];
  # OpenVR's client library is compiled into the mod from source (linux/CMakeLists.txt).
  cmakeFlags = [ "-DX4VR_LINUX=ON" "-DOPENVR_SOURCE_DIR=${pkgs.openvr.src}" ];
  doCheck = true; # the Linux tests

  meta = {
    description = "Stereo VR for the native Linux build of X4: Foundations (experimental)";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "x4vr";
  };
}
