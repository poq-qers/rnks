#!/bin/bash

gcc -o send send.c functions.c timer.c -Wall
gcc -o rec rec.c functions.c timer.c -Wall
