# Multi-Process-Image-Stitcher

This project implements a multi-process image reconstruction system. The program requests random image segments from a server, writes one copy of the segment to a shared ring buffer for inter-process communication, and then concatenates all segments into a complete image.

The user is able to specify the size of the buffer, number of producers, number of consumers, sleep time before consumers process data, and the specific image to downlnoad from the servers.
