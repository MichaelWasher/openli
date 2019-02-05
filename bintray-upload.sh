#!/bin/bash

set -x -e -o pipefail

VERSION=${CI_COMMIT_REF_NAME}
PKGVERSION=1
ARCH=amd64

APIKEY=${BINTRAY_API_KEY}

apt-get update
apt-get install -y curl

UBUNTU_DISTS=("xenial" "artful" "bionic")
DEBIAN_DISTS=("stretch" "jessie" "sid" "buster")
PACKAGE_LIST=("openli-collector" "openli-provisioner" "openli-mediator")

for i in "${UBUNTU_DISTS[@]}"
do
        for comp in "${PACKAGE_LIST[@]}"; do
                echo ubuntu_$i/${comp}

                if [ ! -f built-packages/ubuntu_$i/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb ]; then
                        continue
                fi

                curl -T built-packages/ubuntu_$i/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb -u${BINTRAY_USERNAME}:$APIKEY "https://api.bintray.com/content/wand/OpenLI/$comp/$VERSION/pool/$i/main/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb;deb_distribution=$i;deb_component=main;deb_architecture=$ARCH"
        done
done

for i in "${DEBIAN_DISTS[@]}"
do
        for comp in "${PACKAGE_LIST[@]}"; do
                echo debian_$i/${comp}
                if [ ! -f built-packages/debian_$i/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb ]; then
                        continue
                fi

                curl -T built-packages/debian_$i/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb -u${BINTRAY_USERNAME}:$APIKEY "https://api.bintray.com/content/wand/OpenLI/$comp/$VERSION/pool/$i/main/${comp}_${VERSION}-${PKGVERSION}_${ARCH}.deb;deb_distribution=$i;deb_component=main;deb_architecture=$ARCH"
        done
done
