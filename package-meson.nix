{
  stdenv,
  gcc14Stdenv,
  meson,
  ninja,
  pkg-config,
  boost,
  tbb,
  sourceHighlight,
  version,
  src,
}:

# Use GCC 14 stdenv for C++23 support (std::generator)
gcc14Stdenv.mkDerivation {
  pname = "patchelf";
  inherit version src;
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [
    boost
    tbb
    sourceHighlight
  ];
  # Disable tests - some patchelf tests fail on aarch64 but patchnar works
  doCheck = false;
}
