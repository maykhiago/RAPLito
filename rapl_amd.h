#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <stdint.h>

/*define AMD_MSR ENVIRONMENT*/

#define AMD_MSR_PWR_UNIT 0xC0010299
#define AMD_MSR_CORE_ENERGY 0xC001029A
#define AMD_MSR_PACKAGE_ENERGY 0xC001029B
#define AMD_TIME_UNIT_MASK 0xF0000
#define AMD_ENERGY_UNIT_MASK 0x1F00
#define AMD_POWER_UNIT_MASK 0xF
#define STRING_BUFFER 1024
#define MAX_CPUS                128 //1024
#define MAX_PACKAGES            4 //16


/*define AURORA environment*/

#define MAX_KERNEL              61
#define MAX_THREADS             32
#define PERFORMANCE             0
#define ENERGY                  1
#define EDP                     2


#define END                     10
#define S0                      0
#define S1                      1
#define S2                      2
#define S3                      3
#define REPEAT                  4

typedef struct{
        short int numThreads;
        short int numCores;
        short int bestThread;
        short int auroraMetric;
        short int state;
        short int lastThread;
        short int startThreads;
	      int steps;
        short int pass;
        double bestResult, initResult, lastResult, bestTime, total_region_perf;
        long long kernelBefore[MAX_PACKAGES];
        long long kernelAfter[MAX_PACKAGES];

}typeFrame;

//Methods
void rapl_init(void);
void detect_packages(void);
void start_rapl_sysfs(void); //AMD aurora_start_amd_msr()
double end_rapl_sysfs(void); //AMD aurora_end_amd_msr()
