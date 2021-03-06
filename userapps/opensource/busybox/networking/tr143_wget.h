#ifndef _TR143_WGET_H
#define  _TR143_WGET_H
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

//#define   "http_ftp_config"

#define LINE_SIZE 512


int write_utc_downTime(char *s );
int write_utc_upTime(char *s );
int write_tr143cfg(char *s);

int Http_downROMTime( void );
int Http_downBOMTime( void );
int Http_downEOMTime( void );
int Http_downRequestTime( void );
int Http_downResponseTime( void );
int Http_downTotalReceived( int totalReceived);
int Http_downTestReceived( int testReceived);
int Http_upROMTime( void );
int Http_upBOMTime( void );
int Http_upEOMTime( void );
int Http_upRequestTime( void );
int Http_upResponseTime( void );
int Http_upTestReceived(int testReceived);
int Http_upTotalReceived(int totalReceived);
int write_config(const char * str1);

#endif