# Multi-Process-Image-Stitcher

This project implements a multi-process image reconstruction system. The program requests random image segments from a server, writes one copy of the segment to a shared ring buffer for inter-process communication, and then concatenates all segments into a complete image.
