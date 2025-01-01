#!/bin/bash

gcc -o send send.c functions.c -Wall
gcc -o rec rec.c functions.c -Wall
