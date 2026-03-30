#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>

void print_usage(char *progname)
{
	printf("%s usage: \n",progname);
	printf(" -i(--interval): Time interval. \n");
	printf(" -s(--socket): server IP address. \n");
	printf("  -h(--Help): print this help information.\n");
}

int ds18b20_get_temperature(float *temp)
{
    	char            w1_path[50] = "/sys/bus/w1/devices/";
    	char            chip[20];
    	char            buf[128];
    	DIR            *dirp;
    	struct dirent  *direntp;
    	int             fd =-1;
    	char           *ptr;
    	float           value;
    	int             found = 0;
    	int             rv = 0;

    	if( !temp )
    	{
        	return -1;
    	}

    

   	 if((dirp = opendir(w1_path)) == NULL)
    	{
        	printf("opendir error: %s\n", strerror(errno));
       	 	return -2;
    	}

    	while((direntp = readdir(dirp)) != NULL)
    	{
        	if(strstr(direntp->d_name,"28-"))
        	{
            
            	strcpy(chip, direntp->d_name);
            	found = 1;
            	break;
        }
    	}
    	closedir(dirp);

    	if( !found )
    	{
        	printf("Can not find ds18b20 in %s\n", w1_path);
        	return -3;
    	}


    

    	strncat(w1_path, chip, sizeof(w1_path)-strlen(w1_path));
    	strncat(w1_path, "/w1_slave", sizeof(w1_path)-strlen(w1_path));


  
    	if( (fd=open(w1_path, O_RDONLY)) < 0 )
    	{
        	printf("open %s error: %s\n", w1_path, strerror(errno));
        	return -4;
    	}

    	if(read(fd, buf, sizeof(buf)) < 0)
    	{
        	printf("read %s error: %s\n", w1_path, strerror(errno));
        	rv = -5;
        	goto cleanup;
    	}

    	ptr = strstr(buf, "t=");
    	if( !ptr )
    	{
        	printf("ERROR: Can not get temperature\n");
        	rv = -6;
        	goto cleanup;
    	}

    	ptr+=2;

    	*temp = atof(ptr)/1000;

cleanup:
    close(fd);	
}

int get_devid(char *id,int len);

int main(int argc,char **argv)
{
	int 			clifd=-1;
	int 			rv=-1;
	char 			buf[1024];
	char    		*servip="127.0.0.1";
	int 			port=6666;
	struct sockaddr_in	serv_addr;
	float 			temp;
	int 			interval=10;

	char 			report[100]; 

	int 			ch;
	struct option		opts[]={
		{"interval", required_argument, NULL, 'I'},
		{"ipaddr", required_argument, NULL, 'i'},
		{"port", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{0,0,0,0}
	};
	char 			device_id[20];
	char 			current_time[30];
	struct addrinfo 	hints, *result;	

	while((ch=getopt_long(argc,argv,"I:i:p:h",opts,NULL))!=-1)
	{
		switch(ch)
		{
			case 'I':
				interval=atoi(optarg);
				break;

			case 'i':
				servip=optarg;
				break;
			
			case 'p':
				port=atoi(optarg);
				break;
			case 'h':
				print_usage(argv[0]);
				return 0;
		}
	}

	if(!servip || !port)
	{
		print_usage(argv[0]);
		return 1;
	}

	clifd=socket(AF_INET,SOCK_STREAM,0);
	if(clifd < 0)
	{
		printf("creat socket failure: %s\n",strerror(errno));
		return 2;
	}

	memset(&serv_addr,0,sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int gai_ret = getaddrinfo(servip, NULL, &hints, &result);
	if (gai_ret != 0)
	{
		printf("DNS resolution failure: %s\n", gai_strerror(gai_ret));
		return 3;
	}
	struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
	memcpy(&serv_addr.sin_addr, &addr->sin_addr, sizeof(addr->sin_addr));

	char ip_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
	printf("DNS resolution success!: %s -> %s\n", servip, ip_str);

	freeaddrinfo(result);

	if(connect(clifd, (struct sockaddr *)&serv_addr, sizeof(serv_addr))  < 0)
	{
		printf("connect to sever [%s,%d] failure: %s\n",servip,port,strerror(errno));
		return 4;
	}

	while(1)
	{
		if(ds18b20_get_temperature(&temp)<0)
		{
			printf("Get ds18b20 temperature failure !\n");
			return 0;
		}

		get_devid(device_id, 20);

		time_t now = time(NULL);
		struct tm *t = localtime(&now);
    		strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", t);

		snprintf(report, sizeof(report), "%s,%s,%.2f", device_id, current_time, temp);

		if((rv=write(clifd,report, strlen(report))<0))
		{
			printf("Write data to server [%s:%d] failure: %s\n", servip, port, strerror(errno));
			goto cleanup;
		}

		printf("ds18b20 temperature: %s ℃  send ok \n",report);

		sleep(interval);

	}
cleanup:
    	close(clifd);
    	return 0;
		

}

int get_devid(char *id,int len)
{
	int sn=1;
	snprintf(id,len,"RPI%03d",sn);
	return 0;
}
