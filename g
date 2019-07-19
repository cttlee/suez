export ARCH=arm64
export SUBARCH=arm64
export PATH=$PATH:/home/cttlee/aarch64/bin
export CROSS_COMPILE=aarch64-linux-android-
make suez_defconfig
cov-build --dir cov-int make -j8
zip -r suez.zip cov-int
