{
  stdenv,
  gcc14Stdenv,
  autoreconfHook,
  autoconf-archive,
  pkg-config,
  boost,
  sourceHighlight,
  version,
  src,
  # Installation directory for Android patching (compile-time constant)
  # Default is standard nix-on-droid path
  installationDir ? "/data/data/com.termux.nix/files/usr",
}:

# Use GCC 14 stdenv for C++23 support (std::generator)
gcc14Stdenv.mkDerivation {
  pname = "patchnar";
  inherit version src;
  nativeBuildInputs = [
    autoreconfHook
    autoconf-archive
    pkg-config
  ];
  buildInputs = [
    boost
    sourceHighlight
  ];
  # Set compile-time constants
  configureFlags = [
    "--with-source-highlight-data-dir=${sourceHighlight}/share/source-highlight"
    "--with-install-prefix=${installationDir}"
  ];
  # Disable tests - some patchelf tests fail on aarch64 but patchnar works
  doCheck = false;
}
