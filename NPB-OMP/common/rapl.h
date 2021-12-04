#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

using namespace std;

#define PERIODO  60 /* a cada segundo */
#define MAX_READ 60


/*define RAPL Environment*/

#define CPU_SANDYBRIDGE         42
#define CPU_SANDYBRIDGE_EP      45
#define CPU_IVYBRIDGE           58
#define CPU_IVYBRIDGE_EP        62
#define CPU_HASWELL             60      // 70 too?
#define CPU_HASWELL_EP          63
#define CPU_HASWELL_1           69
#define CPU_BROADWELL           61      // 71 too?
#define CPU_BROADWELL_EP        79
#define CPU_BROADWELL_DE        86
#define CPU_SKYLAKE             78
#define CPU_SKYLAKE_1           94
#define NUM_RAPL_DOMAINS        4
#define MAX_CPUS                128 //1024
#define MAX_PACKAGES            4 //16

void detect_max_energy_range_uj(void);

void rapl_init(void);
void rapl_destructor(void);
void detect_cpu(void);
void detect_packages(void);
void start_rapl_sysfs(void);
void start_rapl_sysfs_global(void);
double end_rapl_sysfs(void);

/*-----------------------------*/
void ALARMhandler(int);
double end_rapl_parcial_reading();
/*---------------------------*/
