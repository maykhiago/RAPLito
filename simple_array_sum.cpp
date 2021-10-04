#include <stdio.h>
#include "rapl.h"

int main(int argc, const char * argv[])
{
		//papito_init();
		rapl_init();

		int K = 128;

		//auto ans = time(N);
		int N = (K*1024)/4; // Byte to Kilo

		int array[N];

		// Initialize the elements
		for (int i=0; i<N; ++i)
			array[i] = i;

		//papito_start();
		start_rapl_sysfs();

		// Run:
		long long int sum = 0;
		for (int i = 0; i < N; i++)
		{
			sum += array[i];
		}

		//papito_end();
		double energy_curr = end_rapl_sysfs();

		printf("%d\t%lld\t%d\n", N, sum, N*4);
		printf("Energy: %.4f\n", energy_curr);
}
