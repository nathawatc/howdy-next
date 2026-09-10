{
  lib,
  stdenv,
  fetchFromGitHub,
  cmake,
  pkg-config,
  gtk3,
  libjpeg,
  libpng,
  openblas,
  zlib,
}:

stdenv.mkDerivation {
  pname = "opencv5";
  version = "5.0.0";

  src = fetchFromGitHub {
    owner = "opencv";
    repo = "opencv";
    tag = "5.0.0";
    hash = "sha256-fmMHLQ5IHDKztuIaetQj/deCl82pVYh+Dvp8G9Xb5bk=";
  };

  strictDeps = true;

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

  buildInputs = [
    gtk3
    libjpeg
    libpng
    openblas
    zlib
  ];

  env = {
    OpenBLAS = openblas;
    OpenBLAS_HOME = openblas.dev;
  };

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_SKIP_BUILD_RPATH=ON"
    "-DBUILD_LIST=core,imgproc,imgcodecs,dnn,videoio,highgui,objdetect"
    "-DBUILD_SHARED_LIBS=ON"
    "-DBUILD_TESTS=OFF"
    "-DBUILD_PERF_TESTS=OFF"
    "-DBUILD_EXAMPLES=OFF"
    "-DBUILD_DOCS=OFF"
    "-DBUILD_PACKAGE=OFF"
    "-DBUILD_opencv_apps=OFF"
    "-DBUILD_opencv_python3=OFF"
    "-DOPENCV_GENERATE_PKGCONFIG=ON"
    "-DOPENCV_ENABLE_PKG_CONFIG=ON"
    "-DWITH_GTK=ON"
    "-DWITH_GTK_2_X=OFF"
    "-DWITH_QT=OFF"
    "-DWITH_FFMPEG=OFF"
    "-DWITH_GSTREAMER=OFF"
    "-DWITH_V4L=ON"
    "-DWITH_OPENCL=OFF"
    "-DWITH_OPENGL=OFF"
    "-DWITH_IPP=OFF"
    "-DWITH_LAPACK=ON"
    "-DWITH_OPENMP=OFF"
    "-DWITH_PROTOBUF=ON"
    "-DBUILD_PROTOBUF=ON"
    "-DWITH_JPEG=ON"
    "-DWITH_PNG=ON"
    "-DWITH_TIFF=OFF"
    "-DWITH_WEBP=OFF"
    "-DWITH_OPENJPEG=OFF"
    "-DWITH_OPENEXR=OFF"
    "-DWITH_AVIF=OFF"
    "-DWITH_JPEGXL=OFF"
    "-DWITH_EIGEN=OFF"
    "-DWITH_TBB=OFF"
    "-DWITH_ITT=OFF"
  ];

  postInstall = ''
    test -s "$out/lib/pkgconfig/opencv5.pc"
    sed -i "s|{exec_prefix}/$out|{exec_prefix}|;s|{prefix}/$out|{prefix}|" \
      "$out/lib/pkgconfig/opencv5.pc"
  '';

  meta = {
    description = "Open Computer Vision Library 5 with Howdy-required modules";
    homepage = "https://opencv.org/";
    license = lib.licenses.asl20;
    platforms = lib.platforms.linux;
  };
}
