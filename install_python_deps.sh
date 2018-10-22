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
python bootstrap.py
python setup.py install --install-lib $pythondir --install-scripts=$pythondir/bin
popd
rm -rf ./setuptools
popd

# Mako
pushd $currentdir
makover=1.0.7
git clone https://bitbucket.org/zzzeek/mako.git
pushd mako
python setup.py install --install-lib $pythondir --install-scripts=$pythondir/bin
popd

rm -rf ./mako
popd
