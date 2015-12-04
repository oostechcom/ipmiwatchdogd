/*
 * This programe wrote by Sam.Lee ; use to act as IPMC watchdog daemon 
 * to control the IPMC watchdog for ATCA blades.
 *
 * The programme depends on BMC hardware and the ipmi_msghandler,
 * ipmi_si, and ipmi_devintf driver 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * If you have any suggestions / questions , please contact Sam.Lee@emerson.com
 *
 * Copyright Sam
 */


#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <linux/ipmi.h>
#include <linux/ipmi_msgdefs.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <string.h>
#include <syslog.h>


#define DEBUG 			"debug"

#define IPMIDeviceName1 "/dev/ipmi0"
#define IPMIDeviceName2 "/dev/ipmidev/0"
#define IPMIWatchdogFile1  "/etc/init.d/ipmiwatchdog"
#define IPMIWatchdogFile2 "/etc/ipmiwatchdog.conf"
/* Configuration file directory*/
#define ConfigurationFileDir 						"/etc/ipmiwatchdog.conf"


#define APP_NetFn_Req 0x06
#define APP_NetFn_Rsp 0x07

#define Get_Device_ID 0x01
#define  Reset_Watchdog_Timer 0x22
#define Set_Watchdog_Timer 0x24
#define Get_Watchdog_Timer 0x25


/* Used by function ReadConfigurationFile to store the parameter*/
#define CONFIG_LINE_LEN 100

/* Configuration file key works*/
#define IPMI_DAEMON  					"Daemon"
#define IPMI_TIMEOUT					"Timeout"
#define IPMI_PRETIMEOUT				"Pretimeout"
#define IPMI_INTERVAL					"Interval"
#define IPMI_PRETIMEOUTINTERRUPT		"INT_Pretimeout"
#define IPMI_ACTION 					"Action"
#define IPMI_PIDFILE 					"Pidfile"

/* Struct for IPMI watchdog parameter */
typedef struct IpmiWatchdogParameter
        {
        unsigned int TimeoutMsb;
        unsigned int TimeoutLsb;
        unsigned int PreTimeout;
        unsigned int Interval;
        enum TimeoutAction
                {
                NoAction,
                HardReset,
                PowerDown,
                PowerCycle
                }TimeoutAction;

        enum PreTimeouInterrupt
                {
                None,
                SMI,
                NMI,
                MSI
               }PreTimeouInterrupt ;
        }IpmiWatchdogParameter;

/* used to conver the dec to hex LSB. MSB */
typedef union IpmiOctToHex
			{
			unsigned int TimeOutOct;
			unsigned char TimeOutHex[2];
			}IpmiOctToHex;

/* used by the IPMI watchdog struct */ 
/* If there is no configuration file, will use this configuration values*/

int IPMI_Interval = 10, IPMI_Timeout = 600, IPMI_Pretimeout =10, IPMI_PretimeoutInterrupt = 0, IPMI_Action = 1;
char *IPMI_Daemon = NULL,  *IPMI_Pidfile = NULL, *IPMI_debug = NULL;
IpmiOctToHex IPMI_MSB_LSB_TimeOut;


/* Open related files, mainly used by function SendIpmiCommand*/
int OpenIpmiRelatedFile(char *FileDirectory1, char *FileDirectory2)
        {
        int IpmiRelatedFD;
        IpmiRelatedFD=open(FileDirectory1, O_RDWR);
        if ( IpmiRelatedFD == -1) 
                {
                IpmiRelatedFD = open( FileDirectory2, O_RDWR);
                if (IpmiRelatedFD == -1) 
                        {
                        printf( "Cannot open %s nor %s, please check the IPMI drive and your system \n", FileDirectory1, FileDirectory2);
                        return (-1);
                        }
                }
        if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
        	{
                printf("debug in OpenIpmiRelatedFile FD = %d \n", IpmiRelatedFD);
        	}
                return(IpmiRelatedFD);
        }

/* Close the opened file */
void CloseIpmiRelatedFile(int IpmiRelatedFD)
                {
                close(IpmiRelatedFD);
                }

/* Used to format the Get Device ID command, just create the request and return the struct ipmi_req. */
/* This function calls the SendIpmiCommand to send the actual command , no input patameter required */
struct ipmi_req IpmiGetDeviceID()
        {
        static struct ipmi_req                   req;    
        static struct ipmi_system_interface_addr si;
		
         /* Fill  the related address, type and lun. This format used to send the send the IPMI Command to local BMC. */ 
        si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
        si.channel = IPMI_BMC_CHANNEL;
        si.lun = 0;
		
        /* Fill  the related address, len,msgid,netfn and cmd. */ 
        req.addr = (unsigned char *) &si; 
        req.addr_len = sizeof(si);
        req.msgid = 0x2924;    						 /* 0x29 represents I and 0x24  represents D */
        req.msg.netfn = IPMI_NETFN_APP_REQUEST;  	/* Application NetFn 0x06*/
        req.msg.cmd = IPMI_GET_DEVICE_ID_CMD;       /* Get Device ID 0x01*/
        req.msg.data = NULL;            				/* Get Device ID does not requre data*/
        req.msg.data_len = 0;
        
        return req;
   
        }

/* Used to format the Set Watchdog Timer command, just create the request and return the struct ipmi_req. */
/* This function calls the SendIpmiCommand to send the actual command , IPMI watchdog timer patameters required */
/* The parameters passed thru the struct IpmiWatchdogParameter*/

struct ipmi_req IpmiSetWatchDog(struct IpmiWatchdogParameter Parameter)
        {
        static struct ipmi_req                            req;
        static char                     data[6];
        static struct ipmi_system_interface_addr si;

        /* Fill  the related address, type and lun. This format used to send the send the IPMI Command to local BMC. */ 
        si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
        si.channel = IPMI_BMC_CHANNEL;
        si.lun = 0;
		
        /* Fill  the related address, len,msgid,netfn and cmd. */
        req.addr = (void *) &si;
        req.addr_len = sizeof(si);
        req.msgid = 0x3337;     			/* 0x33 represents S and 0x37  represents W */
        req.msg.netfn = APP_NetFn_Req;  	/* Application NetFn 0x06*/
        req.msg.cmd = Set_Watchdog_Timer;  /* Set Watchdog 0x24*/
		
        /* logged on expiration; timer stops automatically when Set Watchdog Timer command is received; watchdog used for SMS/OS */
	/* byte 0 is used to define the timer use*/
		
        data[0] = 4;

	/* The switch is used to filter the related bits for the parameter, detail can be found int the IPMI sepc Set Watchdog Timer Command*/
	/* byte 2 is used to define the Timer Actions bits [6:4] is used by pre-timeout interrupt defination*/
	/* bits [2:0] is used by timeout action */
	/*bytes 5, 6 are used by Initial countdown value.*/
	/*byte 5 represents LSB*/
	/*byte 6 represents MSB*/

	/* Pretimeout action */
        switch (Parameter.PreTimeouInterrupt)
                        {
                        case None: data[1] = data[1] & 0x0f;
                                          data[1] = data[1] | 0x00; /* pre-timeout int in byte2 [6:4] */
                                        break;

                        case SMI: data[1] = data[1] & 0x0f;
                                        data[1] = data[1] | 0x10;
                                        break;

                        case NMI:data[1] = data[1] & 0x0f;
                                        data[1] = data[1] | 0x20;
                                        break;

                        case MSI:data[1] = data[1] & 0x0f;
                                        data[1] = data[1] | 0x30;
                                        break;
                                                }
	/* Timeout action */
                        switch (Parameter.TimeoutAction)
                                                {
                                                case NoAction: data[1] = data[1] & 0xf0;
                                                                   data[1] = data[1] | 0x00; /* timeout action in byte2 [2:0] */
                                                                break;
                                                case HardReset: data[1] = data[1] & 0xf0;
                                                                   data[1] = data[1] | 0x01;
                                                                break;
                                                case PowerDown:data[1] = data[1] & 0xf0;
                                                                   data[1] = data[1] | 0x02;
                                                                break;
                                                case PowerCycle:data[1] = data[1] & 0xf0;
                                                                   data[1] = data[1] | 0x03;
                                                                break;
                                                }
                                data[2] =  Parameter.PreTimeout ; 	/* PreTimeout Interval */
                                data[3] = 0x00 ; 					/* Leave alone the timer expired flags */
                                data[4] = Parameter.TimeoutMsb ;	/* Initial count down LSB 100ms */
                                data[5] = Parameter.TimeoutLsb ; 	/* Initial count down MSB 100ms */
                                req.msg.data = data;                    	/* Provide the data to request*/
                                req.msg.data_len = 6;
                                return req;
                                }

/* Used to format the Reset IPMI Watchdog command, just create the request and return the struct ipmi_req. */
/* This function calls the SendIpmiCommand to send the actual command , no input patameter required */

struct ipmi_req IpmiResetWatchDog()
        {
        static struct ipmi_req                            req;
        static struct ipmi_system_interface_addr si;
		
         /* Fill  the related address, type and lun. This format used to send the send the IPMI Command to local BMC. */ 
        si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
        si.channel = IPMI_BMC_CHANNEL;
        si.lun = 0;
		
        /* Fill  the related address, len,msgid,netfn and cmd. */
        req.addr = (void *) &si;
        req.addr_len = sizeof(si);
        req.msgid = 0x3237;     				/* 0x32 represents R and 0x37  represents W */
        req.msg.netfn = APP_NetFn_Req; 			/* Application NetFn 0x06*/
        req.msg.cmd = Reset_Watchdog_Timer;      /* Reset Watchdog 0x22*/
        req.msg.data = NULL;                    		/* Reset Watchdog does not require data*/
        req.msg.data_len = 0;
        return req;
        }


/* Used to format the Get IPMI Watchdog command, just create the request and return the struct ipmi_req. */
/* This function calls the SendIpmiCommand to send the actual command , no input patameter required */

struct ipmi_req IpmiGetWatchDog()
        {
        static struct ipmi_req                            req;
        static struct ipmi_system_interface_addr si;
		
        /* Fill  the related address, type and lun. This format used to send the send the IPMI Command to local BMC. */
        si.addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
        si.channel = IPMI_BMC_CHANNEL;
        si.lun = 0;
		
        /* Fill  the related address, len,msgid,netfn and cmd. */
        req.addr = (void *) &si;
        req.addr_len = sizeof(si);
        req.msgid = 0x2737;     				/* 0x27 represents G and 0x37  represents W */
        req.msg.netfn = APP_NetFn_Req;  		/* Application NetFn 0x06*/
        req.msg.cmd = Get_Watchdog_Timer;       	/* Reset Watchdog 0x25*/
        req.msg.data = NULL;                    		/* Reset Watchdog does not require data*/
        req.msg.data_len = 0;
        return req;
        }


/* Used to Send the RAW IPMI command, require the format  struct ipmi_req. */
/* The function return the Completion Code for the IPMI command */
/* If Debug Mode opened, the function can return all the response data */

int SendIpmiCommand(struct ipmi_req IPMICmdReq)
                {
                int CompletionCode, fd;
                struct ipmi_req req;

                req = IPMICmdReq;

		/* Open the IPMC device in '/dev/'*/
                fd=OpenIpmiRelatedFile(IPMIDeviceName1, IPMIDeviceName2);

		/* Send the IPMI RAW command, the function require IPMI_MSGHALDER, IPMI_SI, IPMI_DEVINTF modules */
                CompletionCode = ioctl(fd, IPMICTL_SEND_COMMAND,(char *)&req);
		if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
			{

		/* Below is used by Debug mode, the response data will be shown in stdout*/

              int i_response ,  got_one_response = 0, ReturnValue ;
              struct ipmi_recv recv;
              unsigned char data[32];
		struct ipmi_system_interface_addr si;

		/* used by select */
		fd_set SendCommandFds;
		
		/* Wait total 6 seconds for the response */
		struct timeval ResponseTimeout={6,0};		
		
		while (!got_one_response) 
			{
		  	/* Wait for a message. */
		  	FD_ZERO(&SendCommandFds);
		  	FD_SET(fd, &SendCommandFds);
		  	ReturnValue = select(fd+1, &SendCommandFds, NULL, NULL, &ResponseTimeout);
		  	if (ReturnValue == -1) 
		  		{
				if (errno == EINTR)
					{
					continue;
					}
				perror("select");
				printf("No response from BMC \n");
		  		}
			got_one_response = 1 ;
			}
		
               printf("debug - IPMI fd = %d \n", fd);
               recv.msg.data = data;
       		 recv.msg.data_len = sizeof (data);
        	recv.addr = (unsigned char *)&si;
        	recv.addr_len = sizeof (si);
        	CompletionCode = ioctl(fd, IPMICTL_RECEIVE_MSG_TRUNC, &recv);
        	printf("CompletionCode=%d\n",CompletionCode);
        	if (CompletionCode!= 0) 
			{
               	 perror("Error in ioctl IPMICTL_RECEIVE_MSG_TRUNC: ");
        		} 
		else 
			{
                	/* Print the response data*/
				
                	printf("Response :\t\trecv_type = %d; msgid = %d\n", recv.recv_type, recv.msgid);
 
                	printf("Address:\t\t");
               	printf("addr_type=0x%x", si.addr_type);
                	printf("; channel=0x%x", (int)si.channel);
                	printf("; lun=0x%x", (int)si.lun);
                	printf("\n");
 
                	printf("Message:\t\t");
                	printf("NetFn=0x%x", recv.msg.netfn);
                	printf("; cmd=0x%x", recv.msg.cmd);
                	printf("; ResponseData_len=%d", recv.msg.data_len);
                	printf("\n");
 
                	printf("ResponseData:\t\t");

			for (i_response = 0; i_response < recv.msg.data_len; i_response++)
				{
                      	printf("byte:%d 0x%x, ", i_response, (int)recv.msg.data[i_response]);
				}
			
                	printf("\n");
                	}

			}
                CloseIpmiRelatedFile(fd);
                return(CompletionCode); 
                }

/* used by ReadConfigurationFile, check the line if it's valuable*/
 /* This file refer to the watchdog version 5.5*/
static int spool(char *line, int *i, int offset)
{
    for ( (*i) += offset; line[*i] == ' ' || line[*i] == '\t'; (*i)++ );
    if ( line[*i] == '=' )
        (*i)++;
    for ( ; line[*i] == ' ' || line[*i] == '\t'; (*i)++ );
    if ( line[*i] == '\0' )
        return(1);
    else
        return(0);
}

/*Function used to read the configuration from the conf file in firectory defined by 'filename' */
/* This file refer to the watchdog version 5.5*/
static int ReadConfigurationFile(char *file)
	{
	FILE *ReadConfigurationFile;

	/* Open the configuration file with readonly parameter*/
	printf("Trying the configuration file %s \n", ConfigurationFileDir);
    	if ( (ReadConfigurationFile = fopen(ConfigurationFileDir, "r")) == NULL ) 
		{
		printf("There is no configuration file, use default values for IPMI watchdog \n");
        	return(1);
    		}

	/* Check to see the configuration has data or not*/
    	while ( !feof(ReadConfigurationFile) ) 
		{
        	char Configurationline[CONFIG_LINE_LEN];

		/* Read the line from configuration file */
        	if ( fgets(Configurationline, CONFIG_LINE_LEN, ReadConfigurationFile) == NULL ) 
			{
            		if ( !ferror(ReadConfigurationFile) )
            			{	
                		break;
            			}
            		else 
				{
                		return(1);
            			}
        		}
        	else 
			{
            		int i, j;

           	 	/* scan the actual line for an option , first remove the leading blanks*/
            		for ( i = 0; Configurationline[i] == ' ' || Configurationline[i] == '\t'; i++ );

            		/* if the next sign is a '#' we have a comment , so we ignore the configuration line */
            		if ( Configurationline[i] == '#' )
            			{
                		continue;
            			}
					
            		/* also remove the trailing blanks and the \n */
            		for ( j = strlen(Configurationline) - 1; Configurationline[j] == ' ' || Configurationline[j] == '\t' || Configurationline[j] == '\n'; j-- );

			Configurationline[j + 1] = '\0';

            		/* if the line is empty now, we don't have to parse it */
            		if ( strlen(Configurationline + i) == 0 )
            			{
				continue;
            			}
					
            		/* now check for an option , interval first */

			/*Interval */
           		if ( strncmp(Configurationline + i, IPMI_INTERVAL, strlen(IPMI_INTERVAL)) == 0 ) 
				{
                		if ( spool(Configurationline, &i, strlen(IPMI_INTERVAL)) )
                			{
                    			fprintf(stderr, "Ignoring invalid line in config file:\n%s\n", Configurationline);
                			}
				else
                			{
                    			IPMI_Interval = atol(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
						printf(" IPMI_Interval = %d \n", IPMI_Interval);
						}
                			}
            			}
				
		   	/*Timeout */
			else if (strncmp(Configurationline + i, IPMI_TIMEOUT, strlen(IPMI_TIMEOUT)) == 0) 
				{
				if ( spool(Configurationline, &i, strlen(IPMI_TIMEOUT)) )
					{
                    			fprintf(stderr, "Ignoring invalid line in config file:\n%s\n", Configurationline);
					}
               	 	else
                			{
                    			IPMI_Timeout = atol(Configurationline + i);
					IPMI_MSB_LSB_TimeOut.TimeOutOct = IPMI_Timeout;
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
						printf(" IPMI_Timeout = %d \n", IPMI_Timeout);
						printf(" IPMI_MSB_LSB_TimeOut.TimeOutOct = %d \n", IPMI_MSB_LSB_TimeOut.TimeOutOct);
						printf(" IPMI_MSB_LSB_TimeOut MSB = %x \n", (int)IPMI_MSB_LSB_TimeOut.TimeOutHex[1]);
						printf(" IPMI_MSB_LSB_TimeOut.TimeOutOct = %x \n", (int)IPMI_MSB_LSB_TimeOut.TimeOutHex[0]);
						}
                			}	
	    			}
			
			/*Pretimeout */
			else if (strncmp(Configurationline + i, IPMI_PRETIMEOUT, strlen(IPMI_PRETIMEOUT)) == 0) 
				{
	        		if (spool(Configurationline, &i, strlen(IPMI_PRETIMEOUT)))
	        			{
					fprintf(stderr, "Ignoring invalid line in config file:\n%s\n", Configurationline);
	        			}
				else
					{
		        		IPMI_Pretimeout = atol(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
					printf(" IPMI_Pretimeout = %d \n", IPMI_Pretimeout);
						}
					}
	    			}
			
			/*Daemon */
			else if ( strncmp(Configurationline + i, IPMI_DAEMON, strlen(IPMI_DAEMON)) == 0 ) 
				{
				if ( spool(Configurationline, &i, strlen(IPMI_DAEMON)))
					{
					IPMI_Daemon = NULL;
					}
				else
					{
					IPMI_Daemon = strdup(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
						printf(" IPMI_Daemon = %s \n", IPMI_Daemon);
						}
					}
				}
			
			/*PretimeoutInterrupt */
			else if ( strncmp(Configurationline + i, IPMI_PRETIMEOUTINTERRUPT, strlen(IPMI_PRETIMEOUTINTERRUPT)) == 0 ) 
				{
				if ( spool(Configurationline, &i, strlen(IPMI_PRETIMEOUTINTERRUPT)) )
					{
					fprintf(stderr, "Ignoring invalid line in config file:\n%s\n", Configurationline);
					}
				else
					{
					IPMI_PretimeoutInterrupt = atol(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
					printf(" IPMI_PretimeoutInterrupt = %d \n", IPMI_PretimeoutInterrupt);	
						}
					}
				}
			
			/*Action */
			else if ( strncmp(Configurationline + i, IPMI_ACTION, strlen(IPMI_ACTION)) == 0 ) 
				{
				if ( spool(Configurationline, &i, strlen(IPMI_ACTION)) )
					{
					fprintf(stderr, "Ignoring invalid line in config file:\n%s\n", Configurationline);
					}
				else
					{
					IPMI_Action = atol(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
					printf(" IPMI_Action = %d \n", IPMI_Action);
						}		
					}
				}
			
			/*Pidfile */
			else if ( strncmp(Configurationline + i, IPMI_PIDFILE, strlen(IPMI_PIDFILE)) == 0 ) 
				{
				if ( spool(Configurationline, &i, strlen(IPMI_PIDFILE)) )
					{
					IPMI_Pidfile = NULL;
					}
				else
					{
					IPMI_Pidfile = strdup(Configurationline + i);
					if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
						{
					printf(" IPMI_Pidfile = %s \n", IPMI_Pidfile);
						}
					}
				}
		
            	else 
			{
                	fprintf(stderr, "Ignoring config Configurationline: %s\n", Configurationline);
            		}
        	}
    	}

	/* Close the configuration file */
	if ( fclose(ReadConfigurationFile) != 0 ) 
		{
        	return(1);
    		}
}


int main(int argc, char *argv[])
        {
       	pid_t IpmiWatchdogDaemonPid;
	int iForCloseFile, Filenumber = 1024, temp_round=0;
       int Ccode;
       struct ipmi_req req;
	   
	IPMI_debug = argv[1];
	/* for A new PID which used by daemon*/
	IpmiWatchdogDaemonPid = fork();
	if(IpmiWatchdogDaemonPid < 0)
		{
		printf("error in fork IPMI watchdog Daemon\n");
		exit(1);
		}

	/*Paraent exit */
	else if(IpmiWatchdogDaemonPid > 0)
		{
		exit(0);
		}
	
	setsid();
	chdir("/");
	umask(0);

	/*Close all files which is opened by paraents*/
	/*for(iForCloseFile = 0; iForCloseFile <= Filenumber; iForCloseFile++)
		{
		close(iForCloseFile);
		}
	*/
	/* Daemon Code */
	for(;;)
		{
		ReadConfigurationFile(ConfigurationFileDir);

        	/* Verify the setwatchdog command*/
        	IpmiWatchdogParameter parameter;
		
        	parameter.TimeoutMsb = (int)IPMI_MSB_LSB_TimeOut.TimeOutHex[0] ;
        	parameter.TimeoutLsb = (int)IPMI_MSB_LSB_TimeOut.TimeOutHex[1] ;
        	parameter.PreTimeout = IPMI_Pretimeout;
        	parameter.TimeoutAction = IPMI_Action;
        	parameter.PreTimeouInterrupt = IPMI_PretimeoutInterrupt;

		/* For debug*/
		if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
			{
		printf("============== Parameter ================ \n");
		printf("in Function Main - parameter.TimeoutMsb = %x \n", parameter.TimeoutMsb);
		printf("in Function Main - parameter.TimeoutLsb = %x \n", parameter.TimeoutLsb);
		printf("in Function Main - parameter.PreTimeout = %d \n", parameter.PreTimeout);
		printf("in Function Main - parameter.TimeoutAction = %d \n", parameter.TimeoutAction);
		printf("in Function Main - parameter.PreTimeouInterrupt = %d \n", parameter.PreTimeouInterrupt);
		printf("===================================== \n");

	
			}
        	req = IpmiGetDeviceID();
        	Ccode = SendIpmiCommand(req);

		if(Ccode != 0)
			{
			printf(" IpmiGetDeviceID Failed with Ccode = %d , will try for another  4 rounds \n", Ccode);
			for(temp_round = 0 ; temp_round <=5 ; temp_round++)
				{
				sleep(1);
				Ccode = SendIpmiCommand(req);
				if(Ccode == 0)
					{
					goto CcodeEqualToZero;
					}
				}
			printf(" BMC detected failed \n");
			return(1);		
			}
		else
			{
		CcodeEqualToZero:
		if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
			{
		/* First, get the watchdog timer*/
        	req = IpmiGetWatchDog();
              Ccode = SendIpmiCommand(req);
		if(Ccode != 0)
			{
			printf(" IpmiGetWatchDog Failed with Ccode = %d  \n", Ccode);
			}

		/* Set the watchdog timer*/
        	req = IpmiSetWatchDog(parameter);
        	Ccode = SendIpmiCommand(req);
		if(Ccode != 0)
			{
			printf(" IpmiSetWatchDog Failed with Ccode = %d  \n", Ccode);
			}

		/* Verify the watchdog parameters again*/
		req = IpmiGetWatchDog();
		Ccode = SendIpmiCommand(req);
		if(Ccode != 0)
			{
			printf(" IpmiGetWatchDog Failed with Ccode = %d  \n", Ccode);
			}
		
			}

		req = IpmiSetWatchDog(parameter);
        	Ccode = SendIpmiCommand(req);
        	if(Ccode != 0)
			{
			printf(" IpmiSetWatchDog Failed with Ccode = %d  \n", Ccode);
			}

		/* start watchdog */
		req = IpmiResetWatchDog();
		Ccode = SendIpmiCommand(req);
		if(Ccode != 0)
			{
			printf("Start IPMC watchdog Failed with Ccode = %d  \n", Ccode);
			}
       		else 
       			{
			printf("Start IPMC watchdog  ....... Done! \n" );
       			}
		/* reflash watchdog according to the configuration file parameter interval */

		while(IPMI_Interval != 0)
			{
			req = IpmiResetWatchDog();
			Ccode = SendIpmiCommand(req);
       			if(Ccode != 0)
				{
				printf(" Reflash IPMC watchdog Failed with Ccode = %d  \n", Ccode);

				/* Try to reflash the watchdog for 5 times*/
				for(temp_round = 0 ; temp_round <= 5; temp_round++)
					{
					req = IpmiResetWatchDog();
					Ccode = SendIpmiCommand(req);
					if(Ccode == 0)
						{
						break;
						}
					sleep(1);
					}
			
				}
			if(IPMI_debug != NULL && strncmp(IPMI_debug, DEBUG, strlen(DEBUG)) == 0)
				{
			/* we use ms for timeout value, so the actual round is IPMI_Timeout /10 */
			printf("Get watchdog command will be issue 1s a time, total %d times \n", (int)(IPMI_Timeout/10));

			for(temp_round = 0; temp_round <= (IPMI_Timeout/10); temp_round++)
				{
				/*Get IPMI watchdog command will be issue 2s a time*/
				sleep(2) ;
				printf("Time pass %d \n", temp_round);
				req = IpmiGetWatchDog();
				Ccode = SendIpmiCommand(req);
				}
				}
			
			sleep(IPMI_Interval);
			}
			}
		}
        }
