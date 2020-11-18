# /bin/bash
Rscript -e "
pkg <- installed.packages();
req_pkg <- c('arrow',
             'bigrquery',
             'cpp11');
install.packages(req_pkg[!req_pkg %in% pkg]);"

# install protoc and grpc
apt-get install build-essential autoconf libtool pkg-config
git clone -b v1.33.2 https://github.com/grpc/grpc
cd grpc
git submodule update --init
./test/distrib/cpp/run_distrib_test_cmake_module_install_pkgconfig.sh
cd ..
rm -R grpc
git clone https://github.com/meztez/bigrquerystorage
R CMD INSTALL --preclean --no-multiarch --with-keep.source bigrquerystorage
