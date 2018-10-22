#!/usr/bin/env bash

currentdir=`dirname $0`
currentdir=`realpath $currentdir`
pythondir=$currentdir/site-packages

mkdir -p $pythondir
export PYTHONPATH=$PYTHONPATH:$pythondir

# pycparser
pushd $currentdir
git clone https://github.com/eliben/pycparser.git

pushd pycparser
python setup.py install --install-lib $pythondir
popd

rm -rf ./pycparser
popd

pushd $current_dir
git clone https://github.com/pypa/setuptools.git
pushd setuptools
python setup.py install --install-lib $pythondir --install-scripts=$pythondir/bin
popd
rm -rf ./setuptools
popd

# Mako
pushd $currentdir
makover=1.0.7
curl -O https://files.pythonhosted.org/packages/eb/f3/67579bb486517c0d49547f9697e36582cd19dafb5df9e687ed8e22de57fa/Mako-$makover.tar.gz
tar xf Mako-$makover.tar.gz
pushd Mako-$makover
python setup.py install --install-lib $pythondir --install-scripts=$pythondir/bin
popd

rm Mako-$makover.tar.gz
rm -rf ./Mako-$makover
popd
