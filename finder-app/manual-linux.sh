#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
MAKE_CROSS_COMPILE_PREFIX="make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}"

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    echo "Building kernel"
    # Make clean
    $MAKE_CROSS_COMPILE_PREFIX mrproper
    # Configure the kernel
    $MAKE_CROSS_COMPILE_PREFIX defconfig
    # Build the kernel image
    $MAKE_CROSS_COMPILE_PREFIX -j$(nproc) all
    # Modules
    $MAKE_CROSS_COMPILE_PREFIX modules
    # Device Tree
    $MAKE_CROSS_COMPILE_PREFIX dtbs
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

echo "Creating ${OUTDIR}/rootfs"
mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs

echo "Creating necessary base directories"

# TODO: Create necessary base directories
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    echo "Configuring busy box"
    make distclean
    make defconfig
else
    cd busybox
fi

echo "Installing busy box"
# TODO: Make and install busybox
$MAKE_CROSS_COMPILE_PREFIX 
$MAKE_CROSS_COMPILE_PREFIX CONFIG_PREFIX=${OUTDIR}/rootfs install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
# Copy necessary libraries
echo "Adding lib deps to rootfs"
SYSROOT=`${CROSS_COMPILE}gcc -print-sysroot`
cp -a ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/
cp -a ${SYSROOT}/lib64/libc.so.6 ${OUTDIR}/rootfs/lib64/
cp -a ${SYSROOT}/lib64/libm.so.6 ${OUTDIR}/rootfs/lib64/
cp -a ${SYSROOT}/lib64/libresolv.so.2 ${OUTDIR}/rootfs/lib64/

# TODO: Make device nodes
echo "Making device nodes"
cd ${OUTDIR}/rootfs
sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 600 dev/console c 5 1
# TODO: Clean and build the writer utility
echo "Cleaning and building writer utility"
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}
# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
echo "Copying finder related scripts"
mkdir -p ${OUTDIR}/rootfs/home
cp ${FINDER_APP_DIR}/finder.sh ${FINDER_APP_DIR}/finder-test.sh ${FINDER_APP_DIR}/writer ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home
mkdir -p ${OUTDIR}/rootfs/home/conf
cp ${FINDER_APP_DIR}/conf/assignment.txt ${FINDER_APP_DIR}/conf/username.txt ${OUTDIR}/rootfs/home/conf

# TODO: Chown the root directory
sudo chown -R root:root ${OUTDIR}/rootfs
# TODO: Create initramfs.cpio.gz
echo "Creating initramfs.cpio.gz"
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
cd ${OUTDIR}
gzip -f initramfs.cpio 
