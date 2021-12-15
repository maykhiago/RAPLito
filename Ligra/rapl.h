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
#include <stdio.h>
#include <omp.h>
#include <iostream>

/* deal with rapl overflow */
#include <unistd.h>
#include <signal.h>

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

/* Implementation */
// Hiago MGA Rocha (14/12/2021)
static int package_map[MAX_PACKAGES];
static int total_packages=0, total_cores=0;
char rapl_domain_names[NUM_RAPL_DOMAINS][30]= {"energy-cores", "energy-gpu", "energy-pkg", "energy-ram"};
char event_names[MAX_PACKAGES][NUM_RAPL_DOMAINS][256];
char filenames[MAX_PACKAGES][NUM_RAPL_DOMAINS][256];
char packname[MAX_PACKAGES][256];
char tempfile[256];
int valid[MAX_PACKAGES][NUM_RAPL_DOMAINS];
double initGlobalTime = 0.0;

long long kernelBefore[MAX_PACKAGES][NUM_RAPL_DOMAINS];
long long kernelAfter[MAX_PACKAGES][NUM_RAPL_DOMAINS];

double max_energy_range_uj;

/*-----------------------------*/
long long parcial[MAX_READ][MAX_PACKAGES][NUM_RAPL_DOMAINS];
short int read_count_energy = 1;
short int leituras = 0;
/*---------------------------*/

/****** RAPL UTILS ******/
void rapl_init()
{
  /*Initialization of RAPL */
  detect_cpu();
  detect_packages();
  detect_max_energy_range_uj();
  /*End initialization of RAPL */
  start_rapl_sysfs_global(); /* chamar so 1 vez*/
}


/* Function used by the Intel RAPL to detect the CPU Architecture*/
void detect_cpu(){
        FILE *fff;
        int family,model=-1;
        char buffer[BUFSIZ],*result;
        char vendor[BUFSIZ];
        fff=fopen("/proc/cpuinfo","r");
        while(1) {
                result=fgets(buffer,BUFSIZ,fff);
                if (result==NULL)
                        break;
                if (!strncmp(result,"vendor_id",8)) {
                        sscanf(result,"%*s%*s%s",vendor);
                        if (strncmp(vendor,"GenuineIntel",12)) {
                                printf("%s not an Intel chip\n",vendor);
                        }
                }
                if (!strncmp(result,"cpu family",10)) {
                        sscanf(result,"%*s%*s%*s%d",&family);
                        if (family!=6) {
                                printf("Wrong CPU family %d\n",family);
                        }
                }
                if (!strncmp(result,"model",5)) {
                        sscanf(result,"%*s%*s%d",&model);
                }
        }
        fclose(fff);
}

/* Function used by the Intel RAPL to detect the number of cores and CPU sockets*/
void detect_packages(){
        char filename[BUFSIZ];
        FILE *fff;
        int package;
        int i;
        for(i=0;i<MAX_PACKAGES;i++)
                package_map[i]=-1;
        for(i=0;i<MAX_CPUS;i++) {
                sprintf(filename,"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",i);
                fff=fopen(filename,"r");
                if (fff==NULL)
                        break;
                fscanf(fff,"%d",&package);
                fclose(fff);
                if (package_map[package]==-1) {
                        total_packages++;
                        package_map[package]=i;
                }
        }
        total_cores=i;
}

/* Function used by the Intel RAPL to store the actual value of the hardware counter*/
void start_rapl_sysfs_global(void){
        int i,j;
        FILE *fff;

        for(j=0;j<total_packages;j++) {
                i=0;
                sprintf(packname[j],"/sys/class/powercap/intel-rapl/intel-rapl:%d",j);
                sprintf(tempfile,"%s/name",packname[j]);
                fff=fopen(tempfile,"r");
                if (fff==NULL) {
                        fprintf(stderr,"\tCould not open %s\n",tempfile);
                        exit(0);
                }
                fscanf(fff,"%s",event_names[j][i]);
                valid[j][i]=1;
                fclose(fff);
                sprintf(filenames[j][i],"%s/energy_uj",packname[j]);

                /* Handle subdomains */
                for(i=1;i<NUM_RAPL_DOMAINS;i++){
                        sprintf(tempfile,"%s/intel-rapl:%d:%d/name", packname[j],j,i-1);
                        fff=fopen(tempfile,"r");
                        if (fff==NULL) {
                                //fprintf(stderr,"\tCould not open %s\n",tempfile);
                                valid[j][i]=0;
                                continue;
                        }
                        valid[j][i]=1;
                        fscanf(fff,"%s",event_names[j][i]);
                        fclose(fff);
                        sprintf(filenames[j][i],"%s/intel-rapl:%d:%d/energy_uj", packname[j],j,i-1);
                }
        }
}

void cleanAll()
{
    leituras = 0;

    for(int j=0;j<total_packages;j++) 
    {
        for(int i=0;i<NUM_RAPL_DOMAINS;i++) 
        {
            kernelBefore[j][i] = 0.0;
            kernelAfter[j][i] = 0.0;
            for (int x = 0; x < MAX_READ; ++x)
            {
                parcial[x][j][i] = 0;
            }
        }
    }
}

/* Function used by the Intel RAPL to store the actual value of the hardware counter*/
void start_rapl_sysfs(){

    // Mudar depois
    start_rapl_sysfs_global();
    cleanAll(); // limpa todos os valores

    int i,j;
    FILE *fff;
         /* Gather before values */

        for(j=0;j<total_packages;j++) {
                for(i=0;i<NUM_RAPL_DOMAINS;i++) {
                        if(valid[j][i]) {
                                fff=fopen(filenames[j][i],"r");
                                if (fff==NULL) {
                                        fprintf(stderr,"\tError opening %s!\n",filenames[j][i]);
                                }
                                else {
                                        fscanf(fff,"%lld",&kernelBefore[j][i]);
                                        fclose(fff);
                                                                        //printf("%d %d %lld", j, i, kernelBefore[j][i]);
                                }
                        }
                }
        }

        read_count_energy = 1;
        signal(SIGALRM, ALARMhandler); /* install the handler    */
        alarm(PERIODO);                /* set alarm clock       */

}

/* Function used by the Intel RAPL to load the value of the hardware counter and returns the energy consumption*/
double end_rapl_sysfs(){
        int i, j;
        FILE *fff;
        double total=0;
        for(j=0;j<total_packages;j++) {
                for(i=0;i<NUM_RAPL_DOMAINS;i++) {
                        if (valid[j][i]) {
                                fff=fopen(filenames[j][i],"r");
                        if (fff==NULL) {
                                fprintf(stderr,"\tError opening %s!\n",filenames[j][i]);
                        }
                        else {
                                fscanf(fff,"%lld",&kernelAfter[j][i]);
                                fclose(fff);
                        }
                }
                }
        }

        /*********************************************************/

      /*  for(j=0;j<total_packages;j++) {
                for(i=0;i<NUM_RAPL_DOMAINS;i++) {
                        if(valid[j][i]){
                                if(strcmp(event_names[j][i],"core")!=0 && strcmp(event_names[j][i],"uncore")!=0){
                                        double after  = (double)kernelAfter[j][i];
                                        double before = (double)kernelBefore[j][i];
                                        if(before < after){
                                                total += ((after-before)/1000000.0);
                                        }else{
                                                total += (((max_energy_range_uj-before)+ after)/1000000.0);
                                        }
                                }
                        }
                }
        }
                printf("total energy %f \n", total);*/

        /*********************************************************/
        return end_rapl_parcial_reading();
}

double end_rapl_parcial_reading(){
        int i, j, k;
    double total=0;

        for(j=0;j<total_packages;j++) {
        for(i=0;i<NUM_RAPL_DOMAINS;i++) {
                if(valid[j][i]){
                if(strcmp(event_names[j][i],"core")!=0 && strcmp(event_names[j][i],"uncore")!=0){
                                        double before = (double)kernelBefore[j][i];
                        for(k=0;k<leituras;k++) {
                                                double current = (double) parcial[k][j][i];

                                                if(before < current)
                        {
                                                        total += ((current-before)/1000000.0);
                                                }else{
                                                        total += (((max_energy_range_uj-before)+ current)/1000000.0);
                                                }

                                                before = current;
                                        }

                                        double after  = (double)kernelAfter[j][i];
                                        if(before < after){
                                                total += ((after-before)/1000000.0);
                                        }else{
                                                total += (((max_energy_range_uj-before)+ after)/1000000.0);
                                        }

                                }
                }
        }

        //cout << "total (time): " << total << endl;
        }
    return total;
}



void ALARMhandler(int sig) {
    if(read_count_energy){
                int i, j;
        FILE *fff;
        for(j=0;j<total_packages;j++) {
            for(i=0;i<NUM_RAPL_DOMAINS;i++) {
                if (valid[j][i]) {
                        fff=fopen(filenames[j][i],"r");
                    if (fff==NULL) {
                        fprintf(stderr,"\tError opening %s!\n",filenames[j][i]);
                    }else {
                        fscanf(fff,"%lld",&parcial[leituras][j][i]);
                        fclose(fff);
                    }
                }
            }
        }
                alarm(PERIODO);
                leituras++;
        }
}

/*from: /sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/max_energy_range_uj*/
void detect_max_energy_range_uj(){
        long long max;
        FILE * file = fopen("/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/max_energy_range_uj","r");
        fscanf(file, "%lld", &max);
        max_energy_range_uj = (double) max;
        //printf("%.2f\n", max_energy_range_uj);
        fclose(file);
}