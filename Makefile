all:
	g++ -fopenmp *.cpp -o main

clean:
	rm -f *.o main