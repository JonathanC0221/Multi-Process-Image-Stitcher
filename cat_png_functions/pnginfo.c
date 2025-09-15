#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>  /* for printf().  man 3 printf */
#include <stdlib.h> /* for exit().    man 3 exit   */
#include <string.h> /* for strcat().  man strcat   */
#include <arpa/inet.h>
#include "crc.h"

unsigned int is_png(char *fileName)
{
    unsigned char header[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}; //png header
    unsigned char buffer[8] = {}; //buffer for file png header

    FILE *file = fopen(fileName, "rb"); // read file
    if (file == NULL)
    {
        printf("bad file\n");
        return 0;
    }
    else
    {
        fread(buffer, 8, 1, file);
        fclose(file);
        for (int i = 0; i < 8; i++)
        {
            if (header[i] != buffer[i])
            { // doesn't match header, not png
                return 0;
            }
        }
        return 1;
    }
}

unsigned int png_height(char *fileName)
{

    unsigned int height;

    FILE *file = fopen(fileName, "rb"); // read file
    if (file == NULL)
    {
        printf("bad file");
        return 0;
    }
    else
    {
        fseek(file, 20, SEEK_SET); //skip over everything to height from IHDR
        fread(&height, 4, 1, file);
        fclose(file);
        height = ntohl(height); //conversion
        return height;
    }
}

unsigned int png_width(char *fileName)
{

    unsigned int width;

    FILE *file = fopen(fileName, "rb"); // read file
    if (file == NULL)
    {
        printf("bad file");
        return 0;
    }
    else
    {
        fseek(file, 16, SEEK_SET); // move 8 bytes after reading for IHDR
        fread(&width, 4, 1, file); // copy to address directly
        fclose(file);
        width = ntohl(width);
        return width;
    }
}

unsigned int check_corrupt(char *fileName)
{ // returns 1 if corrupt, 0 if not corrupt
    FILE *file = fopen(fileName, "rb");
    if (file == NULL)
    {
        printf("bad file\n");
        return 0;
    }
    else
    {
        //IHDR Chunk
        fseek(file, 12, SEEK_SET); //skip 12 to IHDR type byte
        unsigned char IHDR_Buf [17];
        fread(IHDR_Buf, 17, 1, file); //read IHDR type and data chunk
        unsigned int IHDR_Actual; //for actual crc
        fread(&IHDR_Actual, 4, 1, file);
        IHDR_Actual = ntohl(IHDR_Actual);
        unsigned long IHDR_calc_crc = crc(IHDR_Buf, 17); //calculate crc
        if (IHDR_Actual != IHDR_calc_crc) {
            return 1;
        }

        int data;
        fread(&data, 4, 1, file);
        data = ntohl(data); //data length of IDAT
        data += 4;
        unsigned char IDAT_Buf[data];
        fread(IDAT_Buf, data, 1, file); //read IDAT type and data
        unsigned long IDAT_calc_crc = crc(IDAT_Buf, data);
        unsigned int IDAT_actual;
        fread(&IDAT_actual, 4, 1, file); //actual crc of IDAT
        fclose(file);
        IDAT_actual = ntohl(IDAT_actual);
        if (IDAT_actual == IDAT_calc_crc) {
            return 0;
        } else {
            return 1;
        }
    }
}