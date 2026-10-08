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
  # The commit built and its `git describe` (v0.5.0-3-gdc86c67), from .git: in the package's version,
  # and in x4vr's menu, `x4vr version` and bug reports (linux/build_info.cmake). The source copied
  # below leaves .git out, so the build can't ask git: the commit is read here, `git describe` runs
  # on a copy of .git alone (a small step built first, rerun only when .git changes). Without the
  # working files it can't tell uncommitted changes ("-dirty").
  gitDir = root + "/.git";
  hasGit = builtins.pathExists (gitDir + "/HEAD"); # a directory; not a worktree's .git file
  commit = if hasGit then lib.commitIdFromGitRepo gitDir else "";
  describe = if !hasGit then "" else lib.fileContents (pkgs.runCommand "x4vr-describe" {
    nativeBuildInputs = [ pkgs.git ];
    git = lib.cleanSourceWith {
      src = root;
      filter = path: type:
        let rel = lib.removePrefix (toString root + "/") (toString path);
        in rel == ".git" || lib.hasPrefix ".git/" rel;
    };
  } ''
    git -c safe.directory='*' --git-dir="$git/.git" describe --always > $out
  '');
in
pkgs.stdenv.mkDerivation {
  pname = "x4vr";
  version = if describe != "" then lib.removePrefix "v" describe else "unknown"; # 0.5.0-3-gdc86c67

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
  cmakeFlags = [ "-DX4VR_LINUX=ON" "-DOPENVR_SOURCE_DIR=${pkgs.openvr.src}"
                 "-DX4VR_GIT_COMMIT=${commit}" "-DX4VR_GIT_DESCRIBE=${describe}" ];
  doCheck = true; # the Linux tests

  meta = {
    description = "Stereo VR for the native Linux build of X4: Foundations (experimental)";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "x4vr";
  };
}
