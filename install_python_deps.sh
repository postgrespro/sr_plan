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
curl -O https://files.pythonhosted.org/packages/6e/9c/6a003320b00ef237f94aa74e4ad66c57a7618f6c79d67527136e2544b728/setuptools-40.4.3.zip
unzip setuptools-40.4.3.zip

pushd setuptools-40.4.3
python setup.py install --install-lib $pythondir --install-scripts=$pythondir/bin
popd

rm setuptools-40.4.3.zip
rm -rf ./setuptools-40.4.3
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
