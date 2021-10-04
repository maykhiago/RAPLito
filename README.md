# RAPLito

RAPLito is a simple tool developed to facilitate the use of the Running Average Power Limit (RAPL)(https://01.org/blogs/2014/running-average-power-limit-%E2%80%93-rapl).

## Installing

Just write the following command on the Linux's terminal:

***chmod +x raplito_install.sh***

***./raplito_install.sh***

The above commands install the necessary tools and run the example code ***simple_array_sum.app.cpp*** (to verify if everything is OK).

## Using RAPLito in your code

It is necesary to insert three simple commands in you code (see ***rapl.h***):

***rapl_init()*** Initialize the RAPL lib;

***start_rapl_sysfs()*** Start measuring the energy;

***end_rapl_sysfs()*** Finnish the measurement and print the final results.
