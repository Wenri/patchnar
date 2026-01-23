{
  stdenv,
  meson,
  ninja,
  pkg-config,
  boost,
  tbb,
  source-highlight,
  version,
  src,
}:

stdenv.mkDerivation {
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
    source-highlight
  ];
  doCheck = true;
}
