/*
   Program: uptime-agent.c

   A free Uptime daemon agent for OpenVMS.

   This prototype program pretends to be a linux server to Uptime.
   
   This program is at a "technology demonstrator" level.  It is not meant
   to be a final version, nor has any attempt been made to finalize any
   style or structure since there will probably be more additions and
   modifications if more folks use it.

   This program listens on port 9998 for Uptime commands and responds
   with specific performance information.  There are many ways to do
   this, and I picked this one.  

   The basic idea is that the Uptime server will poll OpenVMS every 5
   minutes, causing this program to be accessed. The program is a
   sequence of "if" statements.  If no match is made, the program just
   falls through and sends a blank buffer back to the requestor.

   Yes, I do realize that it is currently just a giant slab of C code.

   The Uptime server, for whatever reason, wants to open/read/close for 
   each command.  Not sure why, but that's the way it is, so the loop
   is moved from the readvblk to the larger $setmode.

   I have tried to follow my 3 rules of C.  No mallocs. No pointers. No regexes. :-)

   If you want to modify this program, be my guest.
      ...just don't turn QIOs into dumpy sockets
      ...just don't turn easy-to-read arrays into pointers
      ...just don't change the format to K & R style   
                     :-)

   If you want this to run on a VAX, you will have to change some stuff
   (better you than me) (Hey, I miss VAXen too, but it's been 20 years)

   This program was written and tested on VMS 8.4 on Itanium blades. If
   you use anything else YMMV.

   The program uses the $getrmi service, so anything older than 7.3 of
   OpenVMS isn't going to work.

   Special thanks to tcpip$examples, eight-cubed, wasd, mondesi and comp.os.vms

   Compile ONLY with /warn=disable=(misalgndstrct)
   Link with /sysexe to pick up system data cells.

   Program probably needs at least GROUP, WORLD, and maybe PHY_IO privs.

   There is a small section of variables to be loaded by hand until I either
   read from a config file or find a way to retrieve the info. Just CPU speed
   and CPU cache size and CPU name.

   There is a shiny brass lamp nearby.
*/


/* Includes */
#include <descrip.h>         /* define OpenVMS descriptors           */
#include <efndef.h>          /* define 'EFN$C_ENF' event flag        */
#include <in.h>              /* define internet related constants,   */
#include <inet.h>            /* define network address info          */
#include <iodef.h>           /* define i/o function codes            */
#include <netdb.h>           /* define network database library info */
#include <ssdef.h>           /* define system service status codes   */
#include <starlet.h>         /* define system service calls          */
#include <stdio.h>           /* define standard i/o functions        */
#include <stdlib.h>          /* define standard library functions    */
#include <string.h>          /* define string handling functions     */
#include <stsdef.h>          /* define condition value fields        */
#include <dvidef>
#include <ints>
#include <devdef>
#include <dcdef>
#include <lnmdef>
#include <psldef>
#include <jpidef>
#include <rmidef>
#include <tcpip$inetdef.h>   /* define tcp/ip network constants,     */
#include <iledef>
#include <syidef>
#define terminator 0,0,0,0
                             /* structures, and functions            */
#include lib$routines
#include str$routines

globalref long int SCH$GI_FREECNT;

/* Constants */
#define  SERV_BACKLOG   100  /* server backlog                  */
#define  SERV_PORTNUM   9998 /* The default Uptime port number  */

#define NMA$C_CTLIN_ZER 0000
#define IO$M_RD_64COUNT 0X4000

/* Structures */
struct iosb {                /* i/o status block                */
    unsigned short status;   /* i/o completion status           */
    unsigned short bytcnt;   /* bytes transferred if read/write */
    void *details;           /* address of buffer or parameter  */
};

struct itemlst_2 {           /* item-list 2 descriptor/element  */
    unsigned short length;   /* length                          */
    unsigned short type;     /* parameter type                  */
    void *address;           /* address of item list            */
};

struct itemlst_3 {           /* item-list 3 descriptor/element  */
    unsigned short length;   /* length                          */
    unsigned short type;     /* parameter type                  */
    void *address;           /* address of item list            */
    unsigned int *retlen;    /* address of returned length      */
};

struct sockchar {             /* socket characteristics buffer  */
    unsigned short prot;      /* protocol                       */
    unsigned char type;       /* type                           */
    unsigned char af;         /* address format                 */
};

struct uptimeiosb {           /* Generic iosb shared by system service calls */
   short int   iosb_status;
   short int   byt_cnt;
   int         unused;
} efn_iosb, rmi_iosb, syi_iosb, send_iosb, qio_iosb, jpi_iosb, dvi_iosb;


/* Return time in Uptime required format */
static int r0_status;
char up_time[12];
static unsigned short int numvec[7];
static unsigned __int64 time_now;


/********DISK STUFF********/
#define DVS$_DEVCLASS 1
#define DEV$V_MNT 19
#define DEV$V_SSM  6
#define terminator 0,0,0,0

int float_type = 4;
int status;
int i;

/*************** Scanlist Item Definitions  *************************/
static unsigned context[2] = {0,0}, net_context[2]  = {0,0}, devclass;
static ile3 scanlist_items[] = {{ sizeof(devclass), DVS$_DEVCLASS, &devclass, 0}, {terminator} };

   
/***************   Device Item Definitions   **************************/
char  device_name[32];
char  volume_name[32];
$DESCRIPTOR(device_desc, device_name);
static unsigned short maxlen, flen,  mlen, dev_len, dt_len, dclen, dc2len, dqlen, oplen;
static unsigned long  maxfiles, mfree, maxb, devchar, devchar2, used, available, dq_length, op_count;

short retlen;
unsigned long   devshdw, shdwlen;
char dev[80];
char search_devnam[65], return_devnam[65];
char outline[132], diskline[132];

char ip_address[16];


/***** Device charateristcs *****/
float utilization;
$DESCRIPTOR(devnam, dev);
$DESCRIPTOR(devnam_desc,search_devnam);
$DESCRIPTOR(return_desc, return_devnam);
$DESCRIPTOR(dt_desc, volume_name);


/* Translate the IP address */
$DESCRIPTOR(lname_desc,"TCPIP$INET_HOSTADDR");
$DESCRIPTOR(ipa_desc,ip_address);
$DESCRIPTOR(ltab_desc,"LNM$SYSTEM_TABLE");
static const unsigned int flags = LNM$M_CASE_BLIND;
static int maxindex;
static int ip_index=0;
static int iplen;
static const unsigned char acmode = PSL$C_EXEC;

/****** Some CPU constants - will be read from config file eventually *******/
static int cpu_mhz = 1729;
static int cpu_cache = 8192;
char cpu_type[12] = "Itanium";
char cpu_array[2056];
char next_cpu[64];


/*************** System Item Definitions  **************************/
int32   memsize,pagefree,pagetotal,swapfree,swaptotal,page_size;
int64 big_memsize, big_pagetotal;

static unsigned long cpu_count;
char  nodename[15];
char  mynode[15];
int   nodelen;
char  c_util[15];
char  uptime_response[4096]; /* needs to be big enough for the df-k command */

char  sysr_buf1[1024];       /* buffers for the sysinfo response string */
char  sysr_buf2[1024];

char  version[4];
char  archname[32];
char  hw_name[62];

char  c_user[10];
char  c_system[10];
char  c_total[10];
char  c_idle[10];
char prePID[2];
char preName[6] = "Uptime";
char upProcName[15];
$DESCRIPTOR(prcnam_d,upProcName);


/* Define the getrmi CPU variables */
int free_pages;
int runq_size;
unsigned long CPU_Count;
float  system_pct, user_pct, idle_pct, pagefile_pct, swapfile_pct;
static int    cpu_cur=0, cpu_com=0, procs=0, lef_wait;

static unsigned __int64  total_last = 0, system_last = 0, user_last = 0, 
                  idle_last = 0,  system_time,     cpuintstk,  cpumpsynch, 
                  cpukernel,      cpuexec,         user_time,  cpusuper, 
                  cpuuser,        cpuidle,         total_time, ActiveCPUs;

unsigned long buffer_size;

/* mpstat variables */
static const mpbuffer_size = 300;
char buffer[300];


/******* Get device/system/cpu information *******/

static ile3 devlist_items[] = { { sizeof(maxfiles),    DVI$_MAXFILES,    &maxfiles,     &maxlen  } , 
                                { sizeof(mfree),       DVI$_FREEBLOCKS,  &mfree,        &flen    } ,
                                { sizeof(maxb),        DVI$_MAXBLOCK,    &maxb,         &mlen    } ,
                                { sizeof(dq_length),   DVI$_QLEN,        &dq_length,    &dqlen   } ,
                                { sizeof(op_count),    DVI$_OPCNT,       &op_count,     &oplen   } ,
                                { sizeof(device_name), DVI$_DEVNAM,      &device_name,  &dev_len } ,
                                { sizeof(volume_name), DVI$_VOLNAM,      &volume_name,  &dt_len  } ,
                                { sizeof(devchar),     DVI$_DEVCHAR,     &devchar,      &dclen   } , 
                                { sizeof(devchar2),    DVI$_DEVCHAR2,    &devchar2,     &dc2len  } , {terminator} };

static ile3  getsyi_items[] = { { sizeof(cpu_count),  SYI$_ACTIVECPU_CNT,    &cpu_count,    0 } ,
                                { sizeof(CPU_Count),  SYI$_POTENTIALCPU_CNT, &CPU_Count,    0 } ,
                                { sizeof(ActiveCPUs), SYI$_ACTIVE_CPU_MASK,  &ActiveCPUs,   0 } ,
                                { sizeof(memsize),    SYI$_MEMSIZE,          &memsize,      0 } ,
                                { sizeof(nodename),   SYI$_NODENAME,         &nodename,     0 } ,
                                { sizeof(pagefree),   SYI$_PAGEFILE_FREE,    &pagefree,     0 } ,
                                { sizeof(pagetotal),  SYI$_PAGEFILE_PAGE,    &pagetotal,    0 } ,
                                { sizeof(swapfree),   SYI$_SWAPFILE_FREE,    &swapfree,     0 } ,
                                { sizeof(swaptotal),  SYI$_SWAPFILE_PAGE,    &swaptotal,    0 } , 
                                { sizeof(archname),   SYI$_ARCH_NAME,        &archname,     0 } , 
                                { sizeof(version),    SYI$_NODE_SWVERS,      &version,      0 } , 
                                { sizeof(page_size),  SYI$_PAGE_SIZE,        &page_size,    0 } , 
                                { sizeof(hw_name),    SYI$_HW_NAME,          &hw_name,      0 } , {terminator} };

static ile3  getrmi_items[] = { { 8,           RMI$_CPUINTSTK,     &cpuintstk,    0 } ,
                                { 8,           RMI$_CPUMPSYNCH,    &cpumpsynch,   0 } ,
                                { 8,           RMI$_CPUKERNEL,     &cpukernel,    0 } ,
                                { 8,           RMI$_CPUEXEC,       &cpuexec,      0 } ,
                                { 8,           RMI$_CPUSUPER,      &cpusuper,     0 } ,
                                { 8,           RMI$_CPUUSER,       &cpuuser,      0 } ,
                                { 8,           RMI$_CPUIDLE,       &cpuidle,      0 } ,
                                { 4,           RMI$_COM,           &cpu_com,      0 } ,
                                { 4,           RMI$_CUR,           &cpu_cur,      0 } ,
                                { 4,           RMI$_LEF,           &lef_wait,     0 } ,
                                { 4,           RMI$_PROCS,         &procs,        0 } , {terminator } };

static ile3  getmp_items[]  = { { mpbuffer_size, RMI$_MODES, &buffer, 0 }, {terminator } };
        

/***** Known Uptime Requests *****/
char  getversion[3]   = "ver";
char  getsysinfo[7]   = "sysinfo";
char  getdfk[4]       = "df-k";
char  getsadc_cpu[8]  = "sadc_cpu";
char  getmpstat[6]    = "mpstat";
char  getnetstat[7]   = "netstat";
char  gettcpinfo[7]   = "tcpinfo";
char  getpsinfo[6]    = "psinfo";
char  getwhoin[5]     = "whoin";
char  getsadc_disk[9] = "sadc_disk"; /* qlen and % busy only, the rest unavailable */
char  getrexec[5]     = "rexec";     /* this is to run something local and return a value */

char  sadc_cpu_data[132];  /* response string for the sadc_cpu request */

/***** Parameters for psinfo request *****/
char *rSearch=" ";
char *rName;
char *rParam;

/***** parameters for rexec *****/
/* ...also uses rSearch... */
char *up_rexec;
char *up_pass;
char *up_command;
char *up_args;
char stub_answer[4] = "test";

char dpid[9],fpid[9];  /* the fpid comes with a space in front */
FILE * fp;
char line[256];
uint32 cputime = 0;
uint64 logintime;
uint32 timeflag = 0;
int j;

static unsigned int mygrp,mymem;
static int mbr;
static int grp;
static int jobtype,sp_status;

static char myterm[16];
static char jpi_nodename[6];
static char imagename[64];
static char shortname[21];
$DESCRIPTOR(img_desc,imagename);
$DESCRIPTOR(short_desc,shortname);
int pcount, ppgcnt, gpgcnt, total_pages;
unsigned long  mypid, real_pid, wsauth, wssize, wsextent, mpid;
char  username[12];
char  prcname[15];
char  account[8];
float percen_cpu;


/****** Commands we need to spawn to match what Uptime expects ******/
char openWhoin[64];
char openPsinfo[64];
char openDiskBusy[64];

char diskMatch[80];
$DESCRIPTOR(dbusy_desc,diskMatch);
char diskPrefix[80] = "monitor disk/end=\"+0:00:03\"/interval=1/percent/display=";
char diskPostfix[10] = "_dbusy.txt";

char whoMatch[132];
$DESCRIPTOR(whoin_desc,whoMatch);
char whoPrefix[80] = "sh users/nod/nosub/nobatch/nonet/nohead/out=";
char whoPostfix[10] = "_whoin.txt";
char busy[8];
char volname[16];
char node[6];
int whocount;

char cpuProcMatch[132];
char cpuPrefix[87] = "pipe monitor/ending=\"+0:00:03\"/interval=1 proc/topcpu | sear/key=(p=2,s=2) sys$input ";
char cpuPostfix[5] = "/out=";
char cpuPostfile[11] = "_psinfo.txt";
char strPID[8];
static const $DESCRIPTOR(cpu_desc,cpuProcMatch);



/****** Setup for process information response ******/
static ile3 getjpi_items[] = { { 64 , JPI$_IMAGNAME   , &imagename, 0 } , 
                               {  6 , JPI$_NODENAME   , &jpi_nodename , 0 } ,
                               {  8 , JPI$_LOGINTIM   , &logintime, 0 } ,
                               {  4 , JPI$_MODE       , &jobtype  , 0 } ,
                               {  4 , JPI$_CPUTIM     , &cputime  , 0 } ,
                               {  4 , JPI$_PID        , &mypid    , 0 } , 
                               {  4 , JPI$_GRP        , &mygrp    , 0 } ,
                               {  4 , JPI$_MEM        , &mymem    , 0 } , 
                               {  4 , JPI$_WSAUTHEXT  , &wsauth   , 0 } ,
                               {  4 , JPI$_PPGCNT     , &ppgcnt   , 0 } ,
                               {  4 , JPI$_GPGCNT     , &gpgcnt   , 0 } ,
                               {  4 , JPI$_WSSIZE     , &wssize   , 0 } ,
                               {  4 , JPI$_WSEXTENT   , &wsextent , 0 } ,
                               { 12 , JPI$_USERNAME   , &username , 0 } ,
                               { 15 , JPI$_PRCNAM     , &prcname  , 0 } ,
                               {  4 , JPI$_MASTER_PID , &mpid     , 0 } , 
                               {  8 , JPI$_ACCOUNT    , &account  , 0 } , {terminator} };

char up_time[12], login_time[12];
char process_name[15], state[20], prio[20], io[20], day[20], cpu[20], faults[20], pages[20],jpi_flags[20];
char proc_line[132];
unsigned long cpu_seconds;
float mem_percent;

/***** Stuff for whoin command *****/
char wi_username[12];
char wi_node[6];
char wi_count[4];
char wholine[80];

/* Starting network setup stuff */
int  one = 1;                         /* reuseaddr option value           */
int icount;
char command[32];

struct iosb iosb;                     /* i/o status block                 */
unsigned int post_status;             /* system service return status     */
unsigned int efn;
unsigned short conn_channel;          /* connect inet device i/o channel  */
unsigned short uptime_channel;        /* listen inet device i/o channel   */

struct sockchar listen_sockchar;      /* listen socket char buffer        */

unsigned int client_retlen;           /* returned length of client socket */
                                      /* address structure                */
struct sockaddr_in client_addr;       /* client socket address structure  */
struct itemlst_3 uptime_itemlst;      /* uptime item-list 3 descriptor    */

struct sockaddr_in serv_addr;         /* server socket address structure  */
struct itemlst_2 serv_itemlst;        /* server item-list 2 descriptor    */

struct itemlst_2 sockopt_itemlst;     /* sockopt item-list 2 descriptor   */
struct itemlst_2 reuseaddr_itemlst;   /* reuseaddr item-list 2 element    */

char upa_version[] = "up.time agent 6.0.0  linux\n"; 
int  buflen = sizeof( upa_version );  /* length of server data buffer     */

char 
readbuf[80];
int  readbuflen = sizeof (readbuf);

$DESCRIPTOR( inet_device, /* string descriptor with logical   */
   "TCPIP$DEVICE:" );     /* name of internet pseudodevice    */


/* Setup for mpstat via $getrmi per the docs*/
#pragma member_alignment save
#pragma nomember_alignment
typedef struct _CPU_struct {
   unsigned char  cpu_id;
   unsigned int   interrupt;
   unsigned int   mpsynch;
   unsigned int   kernel;
   unsigned int   exec;
   unsigned int   super;
   unsigned int   user;
   unsigned int   reserved;
   unsigned int   idle;
}CPU_struct;
#pragma member_alignment restore

CPU_struct *cpu_counters = NULL;
static unsigned __int64 mpsys[8], mpuser[8], mpidle[8], mptot[8],mpsys_usr[8], mpsys_sys[8], mpsys_idle[8];
static unsigned __int64 mpuser_last[8], mpsys_last[8], mpidle_last[8], mptot_last[8];
float mppct_usr, mppct_sys, mppct_idle;


/* Set up the call for the netstat command */
double dCBR, dCBS, dCSE, dCRE, dCOL;
unsigned short pword;
char *vptr;
char CountBuffer[512];
char netCounters[256];
unsigned __int64 value, BytesRx, BytesTx;
static unsigned __int64 NetBytesReceived,
                        NetBytesSent,
                        NetReceiveErrors,
                        NetSentErrors,
                        NetRetransmits,
                        NetCollisions,
                        CurBytesReceived,
                        CurBytesSent,
                        CurReceiveErrors,
                        CurSentErrors,
                        CurRetransmits,
                        CurCollisions,
                        PrevReceiveErrors,
                        PrevSentErrors,
                        PrevBytesReceived,
                        PrevBytesSent,
                        PrevRetransmits,
                        PrevCollisions;
short int net_chan;
char net_name[5];
static const $DESCRIPTOR(net_desc,net_name);
static const $DESCRIPTOR(CountDesc,CountBuffer);

struct {
   unsigned long bufflen;
   unsigned long buffadd;
} buff_desc;

/* string for tcp retransmit count */
char retranCnt[20];
int64  bytes_memory;


/***** START OF MAIN PROGRAM *****/
/* You are standing in the forest */
int main( void ) {

   /* init listen socket characteristics buffer */
   listen_sockchar.prot = TCPIP$C_TCP;
   listen_sockchar.type = TCPIP$C_STREAM;
   listen_sockchar.af   = TCPIP$C_AF_INET;


   /* init reuseaddr's item-list element */
   reuseaddr_itemlst.length  = sizeof( one );
   reuseaddr_itemlst.type    = TCPIP$C_REUSEADDR;
   reuseaddr_itemlst.address = &one;


   /* init sockopt's item-list descriptor */
   sockopt_itemlst.length  = sizeof( reuseaddr_itemlst );
   sockopt_itemlst.type    = TCPIP$C_SOCKOPT;
   sockopt_itemlst.address = &reuseaddr_itemlst;


   /* init client's item-list descriptor */
   memset( &uptime_itemlst, 0, sizeof(uptime_itemlst) );

   uptime_itemlst.length  = sizeof( client_addr );
   uptime_itemlst.address = &client_addr;
   uptime_itemlst.retlen  = &client_retlen;


   /* init client's socket address structure */
   memset( &client_addr, 0, sizeof(client_addr) );


   /* init server's item-list descriptor */
   serv_itemlst.length  = sizeof( serv_addr );
   serv_itemlst.type    = TCPIP$C_SOCK_NAME;
   serv_itemlst.address = &serv_addr;


   /* init server's socket address structure */
   memset( &serv_addr, 0, sizeof(serv_addr) );

   serv_addr.sin_family      = TCPIP$C_AF_INET;
   serv_addr.sin_port        = htons( SERV_PORTNUM );
   serv_addr.sin_addr.s_addr = TCPIP$C_INADDR_ANY;


   /****** Get our network device ******/
   memset(&net_name,0x00,sizeof(net_name));
   devclass = DC$_SCOM;
   search_devnam[0] = '*';
   devnam_desc.dsc$w_length = strlen(search_devnam);
   post_status = sys$device_scan(&return_desc,&retlen,&devnam_desc,scanlist_items,net_context);
   if ( !(post_status & STS$M_SUCCESS) ) {
      printf( "Failed to find network device.\n" );
      exit( post_status );
   }
   return_devnam[5] = 0x00; /* get rid of the colon and null term the string */
   printf("Network device is: %s %d\n",return_devnam,strlen(return_devnam));
   strcpy(net_name,&return_devnam[1]);  /* get rid of leading underscore */


   /****** Get our IP address ******/
   lib$get_logical(&lname_desc,&ipa_desc,&iplen,&ltab_desc,&maxindex,&ip_index,&acmode,&flags);
   printf("IP address is: %s\n",ip_address);
   memset(&netCounters,0x00,sizeof(netCounters));


   /****** Build the CPU array info requested by Uptime ******/
   memset(&nodename,0x00,sizeof(nodename));
   status = sys$getsyiw(0,0,0,getsyi_items,&syi_iosb,0,0);
   if (!(status & 1)) lib$signal(status);
   if (!(syi_iosb.iosb_status & 1)) lib$signal(syi_iosb.iosb_status);

   memset (&cpu_array,0x00,sizeof(cpu_array));
   for (i=0;i<cpu_count;i++) {
      sprintf(next_cpu,"CPU%d=\"%d 0 0 %d %d %s 0\"\n",i,i,cpu_mhz,cpu_cache,cpu_type);
      strcat(cpu_array,next_cpu); 
   }


   /****** build the temp file name for the whoin command ******/
   sprintf(whoMatch,"%s%s%s",whoPrefix,nodename,whoPostfix);
   whoin_desc.dsc$w_length = strlen(whoMatch);

   /****** build the temp file name for the sadc_disk command ******/
   sprintf(diskMatch,"%s%s%s",diskPrefix,nodename,diskPostfix);
   dbusy_desc.dsc$w_length = strlen(diskMatch);
   printf("diskmatch file: %s\n",diskMatch);


   /* who am i */
   status = sys$getjpiw(0,0,0,getjpi_items,&jpi_iosb,0,0);
   if (!(status & 1)) lib$signal(status);
   if (!( jpi_iosb.iosb_status & 1)) lib$signal(jpi_iosb.iosb_status);

   sprintf(strPID,"%x",mypid);
   strncpy(prePID,strPID,2);

   /* build file handles */
   sprintf(openWhoin,"%s%s",nodename,whoPostfix);
   sprintf(openDiskBusy,"%s%s",nodename,diskPostfix);

   sprintf(cpuProcMatch,"%s%s%s%s%s",cpuPrefix,prePID,cpuPostfix,nodename,cpuPostfile);
   printf("strPID is: %s PrePID is: %s\n",strPID,prePID);

   /* Set the process name */
   strcat(upProcName,preName);
   strcat(upProcName,nodename);
   prcnam_d.dsc$w_length = strlen(upProcName);
   status = sys$setprn(&prcnam_d);
   if (!(status & 1)) lib$signal(status);
   sprintf(openPsinfo,"%s%s",nodename,cpuPostfile);




   /*************** Get an ef for the getrmi call *************************/
   status = lib$get_ef(&efn);
   if (!(status & 1)) lib$signal(status);


   /*************** Get the baseline info from getrmi ************************/
   post_status = sys$getrmi (efn,0,0,getrmi_items,&rmi_iosb,0,0);
   if (!(post_status & 1)) lib$signal(post_status);
   post_status = sys$waitfr(efn);
   if (!(status & 1)) lib$signal(status);
   if (!(rmi_iosb.iosb_status & 1)) lib$signal(rmi_iosb.iosb_status);
   post_status = sys$clref(efn);


   /* get a channel to the template network device for netstat counters */
   post_status = sys$assign(&net_desc,&net_chan,0,0);
   if (!( post_status & 1)) lib$signal(post_status);

   /* get a baseline of network counters */
   /* You're in cobble crawl...*/

   buff_desc.buffadd = (unsigned long) &CountDesc;
   buff_desc.bufflen = sizeof (CountDesc);

   post_status = sys$qiow( 0, net_chan,
                           IO$_SENSECHAR | IO$M_RD_COUNT | IO$M_RD_64COUNT | IO$M_CTRL,
                           &qio_iosb,0,0,0,
                           &CountDesc, 0,0,0,0);
   if (!( post_status & 1)) lib$signal(post_status);
   if (!(qio_iosb.iosb_status & SS$_NORMAL)) lib$signal (qio_iosb.iosb_status);

   /*
   If you're reading this, you're probably thinking wha?  Once upon a time,
   before VMS version 6.0 this was a simple static buffer.  For probably very
   good reasons, DEC turned it into a maze of twisty passages, or more accurately
   a varaible length buffer, with a variety of data sizes. So, you have to walk it..
   Special thanks here to wasd and mondesi demos etc...you know who you are :-)
   */
   vptr = CountBuffer;

   while (vptr < CountBuffer + qio_iosb.byt_cnt) {
      pword = *(unsigned short*)vptr;
      vptr += 2;
      if(!pword) break;
      value = 0;
      if ((pword & 0x0fff) < 200) {
         value =*(unsigned __int64*)vptr;
         vptr += 8;
      }else if ((pword & 0x6000) == 0x6000) {
            value = *(unsigned long*)vptr;
            vptr += 4;
      }else if ((pword & 0x6000) == 0x4000) {
            value = *(unsigned short*)vptr;
            vptr += 2;
      }else if ((pword & 0x6000) == 0x2000) {
            value = *(unsigned short*)vptr;
            vptr++;
      }
      if (pword & 0x1000) {
         value = *(unsigned long*)vptr;
         vptr +=2;
         continue;
      }

      switch(pword & 0x0fff) {
         case 2:
            PrevBytesReceived += value;
            break;
         case 3:
            PrevBytesSent += value;
            break;
         case 21:
            PrevSentErrors += value;
            break;
         case 22:
            PrevReceiveErrors += value;
            break;
         case 24:
            PrevBytesSent += value;
            break;
         case 28:
            PrevCollisions += value;
            break;
         default :
      }
   }


   /* assign device channels */
   post_status = sys$assign( &inet_device,  /* device name */
         &uptime_channel,                   /* i/o channel */
         0,                                 /* access mode */
         0                                  /* not used    */
         );

   if ( (post_status & STS$M_SUCCESS) )

   post_status = sys$assign( &inet_device,  /* device name  */
          &conn_channel,                    /* i/o channel  */
          0,                                /* access mode  */
          0                                 /* not used     */
         );

   if ( !(post_status & STS$M_SUCCESS) ) {
      printf( "Failed to assign i/o channel(s) to TCPIP device\n" );
      exit( status );
   }



   /***** START OF MAIN LOOP FOR UPTIME COMMANDS *****/
   /***** Ready to connect with Uptime, so read forever or until error *****/
   printf("Starting the loop.\n");

   for (;;) { 


      /* reset the response buffer */
      memset(&uptime_response,0x00,sizeof(uptime_response));

      /* create a listen socket */
      post_status = sys$qiow( EFN$C_ENF, /* event flag              */
            uptime_channel,              /* i/o channel             */
            IO$_SETMODE,                 /* i/o function code       */
            &iosb,                       /* i/o status block        */
            0,                           /* ast service routine     */
            0,                           /* ast parameter           */
            &listen_sockchar,            /* p1 - socket char buffer */
            0,                           /* p2                      */
            0,                           /* p3                      */
            0,                           /* p4                      */
            &sockopt_itemlst,            /* p5 - socket options     */
            0                            /* p6                      */
          );

      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to create socket\n" );
         exit( status );
      }

      /*
       bind server's internet address and port number to
       listen socket; set socket as a passive socket
      */
      post_status = sys$qiow( EFN$C_ENF, /* event flag              */
               uptime_channel,           /* i/o channel             */
               IO$_SETMODE,              /* i/o function code       */
               &iosb,                    /* i/o status block        */
               0,                        /* ast service routine     */
               0,                        /* ast parameter           */
               0,                        /* p1                      */
               0,                        /* p2                      */
               &serv_itemlst,            /* p3 - local socket name  */
               SERV_BACKLOG,             /* p4 - connection backlog */
               0,                        /* p5                      */
               0                         /* p6                      */
             );

      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to bind socket\n" );
         exit( status );
      }

      /* accept a connection from a client */
      post_status = sys$qiow( EFN$C_ENF, /* event flag                          */
               uptime_channel,           /* i/o channel                         */
               IO$_ACCESS|IO$M_ACCEPT,   /* i/o function code                   */
               &iosb,                    /* i/o status block                    */
               0,                        /* ast service routine                 */
               0,                        /* ast parameter                       */
               0,                        /* p1                                  */
               0,                        /* p2                                  */
               &uptime_itemlst,          /* p3 - remote socket name             */
               &conn_channel,            /* p4 - i/o channel for new connection */
               0,                        /* p5                                  */
               0                         /* p6                                  */
             );
      
      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to accept client connection\n" );
         exit( status );
      }


      /* read the request */
      memset(&readbuf,0x00,sizeof(readbuf));
      memset(&command,0x00,sizeof(command));
      post_status = sys$qiow( EFN$C_ENF, /* event flag           */
             conn_channel,               /* i/o channel          */
             IO$_READVBLK,               /* i/o function code    */
             &iosb,                      /* i/o status block     */
             0,                          /* ast service routine  */
             0,                          /* ast parameter        */
             readbuf,                    /* p1 - buffer address  */
             readbuflen,                 /* p2 - buffer length   */
             0,                          /* p3 */
             0,                          /* p4 */
             0,                          /* p5 */
             0                           /* p6 */
          );
      
      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
        printf( "No more data to read data from client connection\n" );
        break;
      }

   status = sys$getjpiw(0,0,0,getjpi_items,&jpi_iosb,0,0);
   if (!(status & 1)) lib$signal(status);
   if (!( jpi_iosb.iosb_status & 1)) lib$signal(jpi_iosb.iosb_status);

      printf("\nReceived: %s   ppgcnt: %d\n",readbuf,ppgcnt);

      /* get time in Uptime format */
      r0_status = sys$gettim (&time_now);
      r0_status = sys$numtim (numvec,&time_now);
      sprintf (up_time,"%04hu%02hu%02hu%02hu%02hu",
                numvec[0],numvec[1],numvec[2],numvec[3],numvec[4]);


      /* Build the response based on the request */

      /***** Response for the ver request *****/
      if (strcmp(readbuf,getversion) == 0 ) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         strcat(uptime_response,upa_version);
      }

      /***** Response for the sysinfo request *****/
      if (strcmp(readbuf,getsysinfo) == 0 ) {
         memset(&uptime_response,0x00,sizeof(uptime_response));

         status = sys$getsyiw(0,0,0,getsyi_items,&syi_iosb,0,0);
         if (!(status & 1)) lib$signal(status);
         if (!(syi_iosb.iosb_status & 1)) lib$signal(syi_iosb.iosb_status);


         /* spec says memory info in bytes, but they meant kilobytes....so */
         page_size = page_size / 1000;
         pagetotal = pagetotal;
         memsize = memsize;
         swaptotal = swaptotal;

         big_pagetotal = (int64)pagetotal * page_size;
         big_memsize   = (int64)memsize * page_size;
         swaptotal *= page_size;


         memset(&sysr_buf1,0x00,sizeof(sysr_buf1));
         memset(&sysr_buf2,0x00,sizeof(sysr_buf2));

         sprintf(sysr_buf1,
         "SYSNAME=%s\nDOMAIN=(none)\nARCH=\"%s %s\"\nOSVER=\"%s\"\nNUMCPUS=%d\nMEMSIZE=%Lu\nPAGESIZE=%u\nSWAPSIZE=%u\nGPGSLO=0\nVXVM=\"\"\n",
            nodename,archname,hw_name,version,cpu_count,big_memsize,page_size,swaptotal);
         sprintf(sysr_buf2,"SDS=\"\"\nLVM=\"NO\"\n%sNET0=%s=%s\nVMWARE=0\n",cpu_array,net_name,ip_address);

         strcat(uptime_response,sysr_buf1);
         strcat(uptime_response,sysr_buf2);
      }


      /***** Response for the mpstat request *****/
      if (strcmp(readbuf,getmpstat) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         post_status = sys$getsyiw(0,0,0,getsyi_items,&syi_iosb,0,0);
         if (!(status & 1)) lib$signal(status);
         if (!(syi_iosb.iosb_status & 1)) lib$signal(syi_iosb.iosb_status);

         post_status = sys$getrmi (efn,0,0,getmp_items,&rmi_iosb,0,0);
         if (!(post_status & 1)) lib$signal(post_status); 
         post_status = sys$waitfr(efn);
         if (!(post_status & 1)) lib$signal(post_status); 
         post_status = sys$clref(efn);
         if (!(post_status & 1)) lib$signal(post_status); 

         cpu_counters = (CPU_struct *) ( buffer + 4);  /* per the docs */
         /* Don't save interrupt data as it includes idle which goofs the numbers */
         for (i=0; i<CPU_Count; i++) {
            if  (1 == (1 & (ActiveCPUs >> i))) {
               mpsys[i]  = cpu_counters[i].mpsynch + cpu_counters[i].kernel + cpu_counters[i].exec;
               mpuser[i] = cpu_counters[i].super + cpu_counters[i].user;
               mpidle[i] = cpu_counters[i].idle;
               mptot[i]  = mpuser[i] + mpsys[i] + mpidle[i];

               mppct_usr  = 100.0 * (mpuser[i] - mpuser_last[i]) / (mptot[i] - mptot_last[i]); 
               mppct_sys  = 100.0 * (mpsys[i]  - mpsys_last[i])  / (mptot[i] - mptot_last[i]); 
               mppct_idle = 100.0 * (mpidle[i] - mpidle_last[i]) / (mptot[i] - mptot_last[i]); 

               sprintf (next_cpu,"%s %d 0 0 0 0 0 0 0 0 0 0 0 %5.2f %5.2f 0.0 %5.2f\n",
                        up_time,i,mppct_usr,mppct_sys,mppct_idle);
               strcat(uptime_response,next_cpu);

               mpsys_last[i]  = mpsys[i];
               mpuser_last[i] = mpuser[i];
               mpidle_last[i] = mpidle[i];
               mptot_last[i]  = mptot[i];
            }
         }   
      }


      /***** Response for the sadc_cpu request *****/
      if (strcmp(readbuf,getsadc_cpu) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         strcat(uptime_response,up_time);

         /* Collect and Process CPU/LoadAverage stuff */
         memset (&cpu_com,0x00,sizeof(cpu_com));
         post_status = sys$getrmi (efn,0,0,getrmi_items,&rmi_iosb,0,0);
         if (!(post_status & 1)) lib$signal(post_status); 
         post_status = sys$waitfr(efn);
         if (!(post_status & 1)) lib$signal(post_status); 
         post_status = sys$clref(efn);
         if (!(post_status & 1)) lib$signal(post_status); 

         system_time = cpuintstk + cpumpsynch + cpukernel + cpuexec;
         user_time   = cpusuper + cpuuser;
         total_time  = system_time + user_time + cpuidle;

         user_pct   = 100.0 * (user_time - user_last) / (total_time - total_last);
         system_pct = 100.0 * (system_time - system_last) / (total_time - total_last);
         idle_pct   = 100.0 * (cpuidle - idle_last) / (total_time - total_last);

         sprintf(c_user,"%5.2f",user_pct);
         sprintf(c_system,"%5.2f",system_pct);
         sprintf(c_idle,"%5.2f",idle_pct);

         user_last   = user_time;
         system_last = system_time;
         idle_last   = cpuidle;
         total_last  = total_time;

         free_pages = SCH$GI_FREECNT;
         free_pages = free_pages * 8;  /* find a way to check this, 8k pages usually true*/

         /***** sadc_cpu wants free swapspace so... in kb  *****/
         post_status = sys$getsyiw(0,0,0,getsyi_items,&syi_iosb,0,0);
         if (!(post_status & 1)) lib$signal(post_status);
         if (!(syi_iosb.iosb_status & 1)) lib$signal(syi_iosb.iosb_status);
         page_size = page_size / 1000;
         swapfree = swapfree * page_size;

         runq_size = (cpu_com + cpu_cur) - cpu_count;
         if (runq_size < 0) runq_size = 0; 
         sprintf(sadc_cpu_data,",%d,%d,0,%d,%d,%d,0,0,0,0,0.0,0.0,0,0,0,0.0,0,0,0,%d,%d,%d,%d,0,0,0\n",
            (int)user_pct,(int)system_pct,free_pages,swapfree,runq_size,procs,cpu_cur,runq_size,lef_wait);
         strcat(uptime_response,sadc_cpu_data);
      }


      /***** Response for the df-k request *****/
      if (strcmp(readbuf,getdfk) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         strcat(uptime_response,up_time);
         strcat(uptime_response,"\n"); /* this response has just one datetime */
         context[0] = 0;               /* reset the context each time through */
         context[1] = 0;
         devclass = DC$_DISK;
         search_devnam[0] = '*';
         devnam_desc.dsc$w_length = strlen(search_devnam);
         while(1) {
            post_status = sys$device_scan(&return_desc,&retlen,&devnam_desc,scanlist_items,context);
            if (post_status == SS$_NOMOREDEV) {
               break;
            }
            if (!(post_status & 1)) lib$stop(status);

            post_status = sys$getdviw(0,0,&return_desc,&devlist_items,0,0,0,NULL);  
            if (post_status != SS$_NORMAL) lib$stop(status);
            memset (&outline,0x00,sizeof(outline));
            if (devchar & DEV$M_MNT) {
               if ((devchar2 & DEV$M_SHD) && (devchar2 & DEV$M_VRT) || (!(devchar2 & DEV$M_SHD))) {
                  maxb  = maxb  / 2;       /* 1k pages for Uptime */
                  mfree = mfree / 2;       /* 1k pages for Uptime */
                  used  = (maxb - mfree);
                  available = mfree ;
                  utilization = 100.0 * ((float) (maxb - mfree) / maxb);

                  /* Uptime suggests $ in a device might be problematic */
                  for (i=0;i<strlen(device_name);i++) {
                     if (device_name[i] == '$') device_name[i] = '/';
                  }

                  sprintf(outline,"%s %u %u %u %4.2f%% %s\n",device_name,maxb,used,available,utilization,volume_name);
                  strcat(uptime_response,outline);
               }
            }
         }
      }

      /***** Response for the tcpinfo request *****/
      /* Let's just get the most recent value for tcp retransmits */
      if ((strcmp(readbuf,gettcpinfo) == 0)) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         strcat(uptime_response,up_time);
         strcat(uptime_response,"\n");     /* just one timestamp */
         sprintf(retranCnt,"%Lu\n",PrevCollisions);
         strcat(uptime_response,retranCnt);
      }


      /***** Response for the netstat request *****/
      if ((strcmp(readbuf,getnetstat) == 0 )) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         NetBytesReceived = 0;
         NetBytesSent     = 0;
         NetSentErrors    = 0;
         NetReceiveErrors = 0;
         NetCollisions    = 0;
         NetRetransmits   = 0;

         post_status = sys$qiow( 0, net_chan,
                                 IO$_SENSECHAR | IO$M_RD_COUNT | IO$M_RD_64COUNT | IO$M_CTRL,
                                 &qio_iosb,0,0,0,
                                 &CountDesc, 0,0,0,0);
         if (!( post_status & 1)) lib$signal(post_status);
         if (!(qio_iosb.iosb_status & SS$_NORMAL)) lib$signal (qio_iosb.iosb_status);

         vptr = CountBuffer;

         while (vptr < CountBuffer + qio_iosb.byt_cnt) {
            pword = *(unsigned short*)vptr;
            vptr += 2;
            if(!pword) break;
            value = 0;
            if ((pword & 0x0fff) < 200) {
               value =*(unsigned __int64*)vptr;
               vptr += 8;
            }else if ((pword & 0x6000) == 0x6000) {
                  value = *(unsigned long*)vptr;
                  vptr += 4;
            }else if ((pword & 0x6000) == 0x4000) {
                  value = *(unsigned short*)vptr;
                  vptr += 2;
            }else if ((pword & 0x6000) == 0x2000) {
                  value = *(unsigned short*)vptr;
                  vptr++;
            }
            if (pword & 0x1000) {
               value = *(unsigned long*)vptr;
               vptr +=2;
               continue;
            }

            switch(pword & 0x0fff) {
               case 2:
                  NetBytesReceived += value;
                  CurBytesReceived = NetBytesReceived - PrevBytesReceived;
                  PrevBytesReceived = NetBytesReceived;
                  break;
               case 3:
                  NetBytesSent += value;
                  CurBytesSent = NetBytesSent - PrevBytesSent;
                  PrevBytesSent = NetBytesSent;
                  break;
               case 21:
                  NetSentErrors += value;
                  CurSentErrors = NetSentErrors - PrevSentErrors;
                  PrevSentErrors = NetSentErrors;
                  break;
               case 22:
                  NetReceiveErrors += value;
                  CurReceiveErrors = NetReceiveErrors - PrevReceiveErrors;
                  PrevReceiveErrors = NetReceiveErrors;
                  break;
               case 24:
                  NetRetransmits += value;
                  CurRetransmits = NetRetransmits - PrevRetransmits;
                  PrevRetransmits = NetRetransmits;
                  break;
               case 28:
                  NetCollisions += value;
                  CurCollisions = NetCollisions - PrevCollisions;
                  PrevCollisions = NetCollisions;
                  break;
               default :
            }
         }
         /* Uptime wants per second of a 5 minute window in float (for some reason) */
         dCBR = (float)CurBytesReceived / 300.0;
         dCBS = (float)CurBytesSent / 300.0;
         dCSE = (float)CurSentErrors / 300.0;
         dCRE = (float)CurReceiveErrors / 300.0;
         dCOL = (float)CurCollisions;


         strcat(uptime_response,up_time);
         if ((strcmp(readbuf,getnetstat)) == 0 ) {
            sprintf(netCounters," %s %1.1f %1.1f %1.1f %1.1f %1.1f\n",
               net_name, dCBR,dCBS,dCOL,dCRE,dCSE);
            strcat(uptime_response,netCounters);
         }
      }      


      /***** Response for the sadc_disk request *****/
      if (strcmp(readbuf,getsadc_disk) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         memset(&diskline,0x00,sizeof(diskline));

         status = lib$spawn(&dbusy_desc,0,0,0,0,0,&sp_status,0,0,0,0,0,0);
         if (status != SS$_NORMAL) lib$signal(status);
         fp = fopen(openDiskBusy,"r");
         while (fgets(line,100,fp) != NULL) {
            if ((strstr(line,"$1") != NULL) || (strstr(line,"DS") != NULL)) {
               sscanf(line,"%s %s %s %s",return_devnam,node,volname,busy);
               post_status = sys$getdviw(0,0,&return_desc,&devlist_items,&dvi_iosb,0,0,0);
               if (post_status != SS$_NORMAL) lib$stop(status);
               if(!(dvi_iosb.iosb_status & 1)) lib$stop(status);  
               if (devchar & DEV$M_MNT) {
                  if ((devchar2 & DEV$M_SHD) && (devchar2 & DEV$M_VRT) || (!(devchar2 & DEV$M_SHD))) {
                     /* Uptime suggests $ in a device might be problematic */
                     for (i=0;i<strlen(device_name);i++) {
                        if (device_name[i] == '$') device_name[i] = '/';
                     }
                     sprintf(diskline,"%s %s %s %u 0.0 0.0 0 0.0 0.0\n",up_time,device_name,busy,dq_length);
                     strcat(uptime_response,diskline);
                  }
               }
            }
         }
         fclose(fp);
      }


      /***** Response for the whoin request *****/
      if (strcmp(readbuf,getwhoin) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         status = sys$gettim(&time_now);
         status = sys$numtim (numvec,&time_now);
         sprintf (up_time,"%04hu%02hu%02hu%02hu%02hu\n",
                numvec[0],numvec[1],numvec[2],numvec[3],numvec[4]);

         /* Lazy, spawn the SHOW USERS command to see who is logged in */
         status = lib$spawn(&whoin_desc,0,0,0,0,0,&sp_status,0,0,0,0,0,0);
         whocount = 0;
         fp = fopen (openWhoin,"r");
         strcat(uptime_response,up_time);
         while (fgets(line,100,fp) != NULL) {
            if (strstr(line,"login") == NULL) {
               sscanf(line, "%s %s %s", 
               wi_username,wi_node,wi_count);
               sprintf(wholine,"%s %s\n",wi_username,wi_count);
               whocount++;
            }
            strcat(uptime_response,wholine);
         }
         fclose(fp);
         printf("whoinSize: %d whoinCount: %d\n",strlen(uptime_response),whocount);
      }      

char rexec_response[512];
int isFound;
char sPass[20], sCom[32];
char rexec_command[132];
$DESCRIPTOR(rexec_desc,rexec_command);
char rexecTmpFile[64];
      /***** Response for the rexec request *****/
      up_rexec   = strtok(readbuf,rSearch);
      up_pass    = strtok(NULL,rSearch);
      up_command = strtok(NULL,rSearch);
      up_args    = strtok(NULL,rSearch);
      if (strcmp(up_rexec,getrexec) == 0) {
         memset(&uptime_response,0x00,sizeof(uptime_response));
         memset(&line,0x00,sizeof(line));
         memset(&rexec_response,0x00,sizeof(rexec_response));
         printf("%s %s %s %s\n",up_rexec,up_pass,up_command,up_args);
         isFound = 0;
         fp = fopen("uptimpasswd.txt","r");
         while (fgets(line,120,fp) != NULL) {
            sscanf(line,"%s %s",sPass,sCom);
            if (strstr(line,up_command) != NULL) {
               isFound = 1;
               break;
            }
         }         
         fclose(fp);
         if (isFound == 1) {
            printf("Matched: %s\n",up_command);
            sprintf(rexec_command,"@%s.%s",up_command,up_args);
            printf("Trying to run: %s\n",rexec_command);
            status = lib$spawn(&rexec_desc,0,0,0,0,0,&sp_status,0,0,0,0,0,0); 
            if (!(status & 1)) lib$signal(status);
            sprintf(rexecTmpFile,"%s.tmp",up_command);
            fp = fopen(rexecTmpFile,"r");
            while (fgets(line,100,fp) != NULL) {
               strcat(rexec_response,line);
            }
            fclose(fp);
         }else{
            sprintf(rexec_response,"ERROR no matching procedure %s\n",up_command);
         }
         strcat(uptime_response,rexec_response);
      }


      /***** Response for the psinfo request *****/
      rName  = strtok(readbuf,rSearch);     /* psinfo has an argument */
      rParam = strtok(NULL,rSearch);        /* number of processes to check */
      if (strcmp(rName,getpsinfo) == 0) {

         memset(&uptime_response,0x00,sizeof(uptime_response));

         pcount = strtoul(rParam,NULL,10);  /* probably don't need this anymore */

         /* spawn the special MONITOR PROC/TOPCPU command to create the temp file */
         r0_status = lib$spawn(&cpu_desc,0,0,0,0,0,&sp_status,0,0,0,0,0,0);
         fp = fopen (openPsinfo,"r");

         pcount++;  /* Used to return the count requested, now just what is present (up to 15) */
         while (fgets(line,120,fp) != NULL) {
            if (strstr(line,"SWAPPER") == NULL) {
               sscanf(line, "%s %15c %s",dpid,process_name,cpu);
               real_pid = strtoul(dpid,NULL,16); 
               percen_cpu = strtod(cpu,NULL);
               printf("pid:%s %s %1.1f\n",dpid,process_name,percen_cpu);

               status = sys$getjpiw(0,&real_pid,0,getjpi_items,&jpi_iosb,0,0);
               if (( status != SS$_SUSPENDED) && ( status != SS$_NONEXPR)) { 
                  if (!(status & 1)) lib$signal(status);
                  if (( jpi_iosb.iosb_status != SS$_SUSPENDED) && (jpi_iosb.iosb_status != SS$_NONEXPR)) { 
                     if (!(jpi_iosb.iosb_status & 1)) lib$signal(jpi_iosb.iosb_status);
   
                     status = sys$numtim(numvec,&logintime);
                     sprintf (login_time,"%04hu%02hu%02hu%02hu%02hu",
                              numvec[0],numvec[1],numvec[2],numvec[3],numvec[4]);

                     /* The <> characters confuse HTML in Uptime */
                     for (j=0;j<strlen(account);j++) {
                        if (account[j] == '<') account[j] = '_';
                        if (account[j] == '>') account[j] = '_';
                     }
      
                     /* Embedded spaces in a process name confuses Uptime */
                     for (j=0;j<strlen(prcname);j++) {
                        if (prcname[j] == ' ') prcname[j] = '_';
                     }
                     
                     total_pages = (gpgcnt + ppgcnt) ;
                     mem_percent =  100 * ((float)total_pages / (float)memsize);
                     cpu_seconds = cputime / 100;
                     sprintf(proc_line,"%s %d %d %s %s %d %d %1.1f %1.1f %u 0 %s %s\n",
                     up_time,real_pid,mpid,username,account,wsauth,total_pages,percen_cpu,mem_percent,cpu_seconds,login_time,prcname);
                     strcat(uptime_response,proc_line);
                  }
               }
            }
         } 
         fclose(fp);
         printf("Size of uptime_response is: %d\n",strlen(uptime_response));
      }


      /* Write the response */
      post_status = sys$qiow( EFN$C_ENF, /* event flag            */
               conn_channel,             /* i/o channel           */
               IO$_WRITEVBLK,            /* i/o function code     */
               &iosb,                    /* i/o status block      */
               0,                        /* ast service routine   */
               0,                        /* ast parameter         */
               uptime_response,          /* p1 - buffer address   */
               strlen(uptime_response),  /* p2 - buffer length    */
               0,                        /* p3                    */
               0,                        /* p4                    */
               0,                        /* p5                    */
               0                         /* p6                    */
             );

      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to write data to client connection\n" );
      }


      /* Shut down the socket cleanly between Uptime calls */
      /* shutdown connection socket */
      post_status = sys$qiow( EFN$C_ENF,   /* event flag               */
               conn_channel,               /* i/o channel              */
               IO$_DEACCESS|IO$M_SHUTDOWN, /* i/o function code        */
               &iosb,                      /* i/o status block         */
               0,                          /* ast service routine      */
               0,                          /* ast parameter            */
               0,                          /* p1                       */
               0,                          /* p2                       */
               0,                          /* p3                       */
               TCPIP$C_DSC_ALL,            /* p4 - discard all packets */
               0,                          /* p5                       */
               0                           /* p6                       */
              );
      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to shutdown client connection\n" );
         exit( status );
      }

      /* close connection socket */
      post_status = sys$qiow( EFN$C_ENF,  /* event flag           */
               conn_channel,              /* i/o channel          */
               IO$_DEACCESS,              /* i/o function code    */
               &iosb,                     /* i/o status block     */
               0,                         /* ast service routine  */
               0,                         /* ast parameter        */
               0,                         /* p1                   */
               0,                         /* p2                   */
               0,                         /* p3                   */
               0,                         /* p4                   */
               0,                         /* p5                   */
               0                          /* p6                   */
             );

      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to close socket\n" );
         exit( status );
      }

      /* close listen socket */
      post_status = sys$qiow( EFN$C_ENF,  /* event flag          */
               uptime_channel,            /* i/o channel         */
               IO$_DEACCESS,              /* i/o function code   */
               &iosb,                     /* i/o status block    */
               0,                         /* ast service routine */
               0,                         /* ast parameter       */
               0,                         /* p1                  */
               0,                         /* p2                  */
               0,                         /* p3                  */
               0,                         /* p4                  */
               0,                         /* p5                  */
               0                          /* p6                  */
             );

      if ( post_status & STS$M_SUCCESS ) status = iosb.status;
      if ( !(status & STS$M_SUCCESS) ) {
         printf( "Failed to close socket\n" );
         exit( status );
      }


   } /* end for loop */

   /* Technically, we don't get here anymore, maybe an exit handler would be a good idea */
   /* deassign all device sockets */
   post_status = sys$dassgn( conn_channel );

   if ( (post_status & STS$M_SUCCESS) ) post_status = sys$dassgn( uptime_channel );
   if ( !(post_status & STS$M_SUCCESS) ) {
      printf( "Failed to deassign i/o channel(s) to TCPIP device\n" );
      exit( status );
   }
   printf("Released the socket, re-setting the read/accept.\n");

   exit( EXIT_SUCCESS );
}

