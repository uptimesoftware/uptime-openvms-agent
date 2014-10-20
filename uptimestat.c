/*
Program: nimstat
Author : R Nelson

This program checks the status of the custom cdm probe for VMS.  It is meant to be run
on a VMS cluster.  It checks all of the active nodes of the cluster to find a detached 
process owned by me.  If all four nodes of the cluster are active, it should find four
matches.

It uses sys$process_scan and sys$getjpi to find the information.

*/
#include <stdio>
#include <jpidef>
#include <starlet>
#include <string>
#include <iledef>
#include <pscandef>
#include <assert>
#include <ssdef>
#include <efndef>
#include <descrip>
#include <ints>

#include lib$routines

#define terminator 0,0,0,0

struct iosb {
   short int iosb_status;
   short int byte_cnt;
   int   unused;
} jpi_iosb;

uint32 cputime = 0;
unsigned short timlen;
char timestring[24];
$DESCRIPTOR(time_desc,timestring);
uint64 logintime;
uint32 timeflag = 0;


static unsigned int mygrp,mymem;
static int mbr;
static int grp;
static int jobtype;
static unsigned int ps_ctx = 0;

int pages, gblpages, total_mem;
 
static char myterm[16];
static char nodename[6];
static char imagename[64];
static char shortname[21];
$DESCRIPTOR(img_desc,imagename);
$DESCRIPTOR(short_desc,shortname);

unsigned long status, mypid;


static struct {
   unsigned short int length;
   unsigned short int code;
   unsigned long int val;
   unsigned long int flags;
} psitems[] = {0 , PSCAN$_NODE_CSID , 0, PSCAN$M_NEQ,
               0 , PSCAN$_GRP       , 0, PSCAN$M_EQL,
               0 , PSCAN$_MEM       , 0, PSCAN$M_EQL,
               0,0,0,0 };


static ile3 getjpi_items[] = { { 64 , JPI$_IMAGNAME, &imagename, 0 } , 
                               { 6  , JPI$_NODENAME, &nodename , 0 } ,
                               { 8  , JPI$_LOGINTIM, &logintime, 0 },
                               { 4  , JPI$_MODE    , &jobtype  , 0 } ,
                               { 4  , JPI$_CPUTIM  , &cputime  , 0 } ,
                               { 4  , JPI$_PID     , &mypid    , 0 } , 
                               { 4  , JPI$_GRP     , &mygrp    , 0 } ,
                               { 4  , JPI$_PPGCNT  , &pages    , 0 } ,
                               { 4  , JPI$_GPGCNT  , &gblpages , 0 } ,
                               { 4  , JPI$_MEM     , &mymem    , 0 } , {terminator} };

main () {

   /* Find my group and member IDs for $getjpi */  
   status = sys$getjpiw(0,0,0,getjpi_items,&jpi_iosb,0,0);
   if (!(status & 1)) lib$signal(status);
   if (!(jpi_iosb.iosb_status & 1)) lib$signal(jpi_iosb.iosb_status);

   /* Build the pscan stuff */
   assert (psitems[1].code == PSCAN$_GRP);
   psitems[1].val = mygrp;
   assert (psitems[2].code == PSCAN$_MEM);
   psitems[2].val = mymem;

   /* Build the context for $getjpi */
   status = sys$process_scan(&ps_ctx, &psitems);
   if (!(status & 1)) lib$signal(status);
   status = lib$date_time(&time_desc);
   printf("\n         Checking uptime agent status at: %s\n",timestring);


   printf("\n               Image         Node       Pid       CPUsec        ppgcnt  gpgcnt\n");
   printf("----------------------------------------------------------------------------------\n");

   /* Run the context until all processes found */
   while (TRUE) {
      status = sys$getjpiw (EFN$C_ENF, &ps_ctx, 0, getjpi_items, &jpi_iosb, 0, 0);
      if (jpi_iosb.iosb_status == SS$_NOMOREPROC) {
         printf("----------------------------------------------------------------------------------\n");
         break;
      }else{
         if (!(jpi_iosb.iosb_status & 1)) lib$signal(jpi_iosb.iosb_status);
         total_mem = pages + gblpages;
         lib$trim_filespec(&img_desc,&short_desc,&sizeof(shortname));  /* Just the filename  */
         if (strstr(shortname,"UPTIME-AGENT") != NULL) { 
            status = sys$asctim(&timlen,&time_desc,&logintime,timeflag);  /* Convert binary time */
            printf ("%20s       %6s  %x     %8.1f        %6d  %6d\n",
               shortname,nodename,mypid,(float)cputime/100,pages,gblpages);   
         } 
      }
   }
}

