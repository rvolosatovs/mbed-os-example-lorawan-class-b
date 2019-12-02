with import <nixpkgs> {};
stdenv.mkDerivation {
  name = "env";
  buildInputs = [
    git
    dockerImages.mbed
  ];
}
