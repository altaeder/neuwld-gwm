{
  description = "GalleryWM-oriented fork of neuwld";

  inputs = 
  {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs
      {
        inherit system;
      };

      nativeBuildInputs = with pkgs;
      [
        meson
        ninja
        pkg-config
        wayland-scanner
        doxygen
      ];

      buildInputs = with pkgs;
      [
        # Core neuwld dependencies
        fontconfig
        pixman
        freetype

        # Wayland // DRM
        wayland
        libdrm

        # AMDGPU / OpenGL stack
        xf86-video-amdgpu
        libGL
        libGLU
        mesa
        mesa-gl-headers
        mesa_glu
        libgbm
        libglvnd
        egl-wayland
        egl-gbm
      ];

      neuwld-gwm = pkgs.stdenv.mkDerivation
      {
        pname = "neuwld-gwm";
        version = "0.0";

        src = ./.;

        inherit nativeBuildInputs buildInputs;

        mesonFlags = 
        [
          "-Dwayland=enabled"
          "-Ddrm=enabled"
          "-Ddrivers=amdgpu"
          "-Ddoxygen=disabled"
        ];
      };
    in
    {
      packages.${system}.default = neuwld-gwm;
      
      devShells.${system}.default = pkgs.mkShell
      {
        inherit nativeBuildInputs buildInputs;

        packages = 
        [
          pkgs.gdb
          pkgs.valgrind
        ];
      };
    };
}