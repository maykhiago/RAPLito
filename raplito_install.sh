#!/bin/bash

#Obs: Run the following command to make this script executable
#     chmod +x papito_install.sh

echo ">>>>>>> Compiling example code"
g++ -fopenmp simple_array_sum.cpp -o simple_array_sum

echo ">>>>>>> Running example code"
./simple_array_sum

cd ..

echo ">>>>>>> Cloning PAPI repository"
git clone https://github.com/sbeamer/gapbs.git

echo ">>>>>>> Copying the modified files"
cp PAPIto/GAPBS/*.cpp gapbs/src
cp PAPIto/GAPBS/*.cc gapbs/src
cp PAPIto/GAPBS/*.h gapbs/src
cp PAPIto/GAPBS/*.in gapbs/src
cp PAPIto/GAPBS/Makefile gapbs/

cd gapbs

echo ">>>>>>> Compilling"
make

echo ">>>>>>> Running PageRank Algorithm"
./pr -f test/graphs/4.el -n1

cd ../RAPLito
