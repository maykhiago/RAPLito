#! /bin/bash

export OMP_NUM_THREADS=1
export GOMP_CPU_AFFINITY="0"

#Disable turbo boost
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo

#Set performance governor
for i in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
do
  echo performance > $i
done

#Disable hyperthresding
for i in 1 2 3;
do
  echo 0 | sudo tee /sys/devices/system/cpu/cpu$i/online
  echo 0 | sudo tee /sys/devices/system/cpu/cpu$i/online
  echo 0 | sudo tee /sys/devices/system/cpu/cpu$i/online
done

for x in 1 2 3 4 5 6 7 8 9 10:
do
  ./simple_array_sum_32 >> Results/results32.out
  ./simple_array_sum_64 >> Results/results64.out
  ./simple_array_sum_128 >> Results/results128.out
done

#Enable turbo boost
echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo

#Set powersave governor
for i in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
do
  echo powersave > $i
done

#Enable hyperthresding
for i in 1 2 3;
do
  echo 1 | sudo tee /sys/devices/system/cpu/cpu$i/online
  echo 1 | sudo tee /sys/devices/system/cpu/cpu$i/online
  echo 1 | sudo tee /sys/devices/system/cpu/cpu$i/online
done
