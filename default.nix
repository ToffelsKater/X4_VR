# nix-build: the native Linux build (docs/LINUX_PORT_PLAN.md). Uses the nixpkgs channel by
# default; pass --arg pkgs to use another.
{ pkgs ? import <nixpkgs> { } }:
pkgs.callPackage ./nix/package.nix { }
