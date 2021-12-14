#!/bin/bash

#Obs: Run the following command to make this script executable
#     chmod +x raplito_install.sh

echo ">>>>>>> Compiling example code"
g++ -fopenmp rapl.cpp simple_array_sum.cpp -o simple_array_sum

echo ">>>>>>> Running example code"
./simple_array_sum

cd ..

echo ">>>>>>> Cloning GAPBS repository"
git clone https://github.com/sbeamer/gapbs.git

echo ">>>>>>> Copying the modified files"
cp RAPLito/GAPBS/*.cpp gapbs/src
cp RAPLito/GAPBS/*.cc gapbs/src
cp RAPLito/GAPBS/*.h gapbs/src
#cp RAPLito/GAPBS/*.in gapbs/src
cp RAPLito/GAPBS/Makefile gapbs/

cd gapbs

echo ">>>>>>> Compilling"
make

echo ">>>>>>> Running PageRank Algorithm"
./pr -f test/graphs/4.el -n1

cd ..

echo ">>>>>>> Copying NAS repository"
cp -r RAPLito/NPB-OMP/ .
cd NPB-OMP/

echo ">>>>>>> Compilling"
./compile_all.sh

echo ">>>>>>> Running bt.A"
bin/bt.A

cd ..

export OPENMP=true
echo ">>>>>>> Cloning Ligra repository"
git clone https://github.com/jshun/ligra.git

echo ">>>>>>> Copying the modified files"
cp RAPLito/Ligra/*.h ligra/ligra
cp RAPLito/Ligra/Makefile ligra/apps

cd ligra/apps

echo ">>>>>>> Compilling"
make

echo ">>>>>>> Running BellamFord Algorithm"
./BellmanFord -f ../inputs/rMatGraph_WJ_5_100

cd ../../RAPLito
