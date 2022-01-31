/* File that contains the variable declarations */
#include "rapl.h"
#include <stdio.h>

/*global variables*/

static int package_map[MAX_PACKAGES];
char packname[MAX_PACKAGES][256];
char tempfile[256];
double initGlobalTime = 0.0;
unsigned long int idKernels[MAX_KERNEL];
short int id_actual_region=0;
short int auroraMetric;
short int totalKernels=0;
short int auroraTotalPackages=0;
short int auroraTotalCores=0;

typeFrame auroraKernels[MAX_KERNEL];

void detect_packages()
{
	char filename[STRING_BUFFER];
	FILE *fff;
	int package;
	int i;

	for(i=0;i<MAX_PACKAGES;i++)
    package_map[i]=-1;

	for(i=0;i<MAX_CPUS;i++)
  {
		sprintf(filename,"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",i);
		fff=fopen(filename,"r");

		if (fff==NULL)
      break;

		fscanf(fff,"%d",&package);
		fclose(fff);

		if (package_map[package]==-1)
    {
			auroraTotalPackages++;
			package_map[package]=i;
		}
	}
}

void start_rapl_sysfs()
{
	char msr_filename[STRING_BUFFER];
	int fd;

	sprintf(msr_filename, "/dev/cpu/0/msr");
	fd = open(msr_filename, O_RDONLY);
	if ( fd < 0 )
  {
		if ( errno == ENXIO )
    {
			fprintf(stderr, "rdmsr: No CPU 0\n");
			exit(2);
		}
    else if ( errno == EIO )
    {
			fprintf(stderr, "rdmsr: CPU 0 doesn't support MSRs\n");
			exit(3);
		}
    else
    {
			perror("rdmsr:open");
			fprintf(stderr,"Trying to open %s\n",msr_filename);
			exit(127);
		}
	}
	uint64_t data;
	pread(fd, &data, sizeof data, AMD_MSR_PACKAGE_ENERGY);
	//auroraKernels[id_actual_region].kernelBefore[0] = read_msr(fd, AMD_MSR_PACKAGE_ENERGY);
	auroraKernels[id_actual_region].kernelBefore[0] = (long long) data;
}

double end_rapl_sysfs(){
	char msr_filename[STRING_BUFFER];
        int fd;
        sprintf(msr_filename, "/dev/cpu/0/msr");
        fd = open(msr_filename, O_RDONLY);
		uint64_t data;
        pread(fd, &data, sizeof data, AMD_MSR_PWR_UNIT);
	int core_energy_units = (long long) data;
	unsigned int energy_unit = (core_energy_units & AMD_ENERGY_UNIT_MASK) >> 8;
	pread(fd, &data, sizeof data, AMD_MSR_PACKAGE_ENERGY);
	auroraKernels[id_actual_region].kernelAfter[0] = (long long) data;
	double result = (auroraKernels[id_actual_region].kernelAfter[0] - auroraKernels[id_actual_region].kernelBefore[0])*pow(0.5,(float)(energy_unit));
	return result;
}

void rapl_init()
{
        detect_packages();
}
