
#ifndef _VISUAL_GPS_H
#define _VISUAL_GPS_H

#include <curl/curl.h>
#include <stdio.h>

int download_file(char* url, char* filename);
void get_image_from_gps(double latitude /* signed */, double longitude /* signed */, int map_width, int  map_height, char* filename /* output */, char *key, int angle);

#endif
