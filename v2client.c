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
#include <sqlite3.h>

sqlite3     *cache_db=NULL;

int init_cache_database(void)
{
    int rc;
    char    *err_msg=NULL;

    rc=sqlite3_open("temperature_cache.db",&cache_db);
    if(rc!=SQLITE_OK)
    {
        printf("Failed to open cache database:%s\n",sqlite3_errmsg(cache_db));
        return -1;
    }

    char *sql=
        "CREATE TABLE IF NOT EXISTS temperature_cache ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"  
        "device_id TEXT NOT NULL,"               
        "time TEXT NOT NULL,"                    
        "temp REAL NOT NULL,"                    
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP);";
    rc=sqlite3_exec(cache_db,sql,NULL,0,&err_msg);
    if(rc!=SQLITE_OK)
    {
        printf("Failed to create cache database:%s\n",err_msg);
        sqlite3_free(err_msg);
        return -2;
    }

    printf("Cache database initialized successfully\n");
    return 0;
}

int save_to_cache(const char *device_id,const char *time_str,float temp)
{
    char        sql[512];
    char        *err_msg=NULL;
    int     rc;

    snprintf(sql,sizeof(sql),
            "INSERT INTO temperature_cache(device_id,time,temp) "
            "VALUES ('%s','%s',%.2f);",
            device_id,time_str,temp);
    rc=sqlite3_exec(cache_db,sql,NULL,0,&err_msg);
    if(rc!=SQLITE_OK)
    {
        printf("Failed to save to cache :%s\n",err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    printf("Data saved to cache: device=%s, time=%s, temp=%.2f\n",device_id,time_str,temp);
    return 0;
}

int connect_to_server(const char *servip,int port,int max_retries,int retry_interval)
{
    int sockfd = -1;
    int retry_count = 0;
    struct sockaddr_in serv_addr;
    struct addrinfo hints, *result;
    
    while(retry_count < max_retries && sockfd < 0)
    {
        sockfd=socket(AF_INET,SOCK_STREAM,0);
        if(sockfd < 0)
        {
            printf("Failed to create socket: %s\n",strerror(errno));
            retry_count++;
            if(retry_count < max_retries)
            {
                printf("Retrying in %d seconds...\n", retry_interval);
                sleep(retry_interval);
            }
            continue;
        }
        
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        int gai_ret = getaddrinfo(servip, NULL, &hints, &result);
        if(gai_ret != 0)
        {
            printf("DNS resolution failure: %s\n", gai_strerror(gai_ret));
            close(sockfd);
            sockfd = -1;
            retry_count++;
            if(retry_count < max_retries)
            {
                printf("Retrying in %d seconds...\n", retry_interval);
                sleep(retry_interval);
            }
            continue;
        }
        
        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        memcpy(&serv_addr.sin_addr, &addr->sin_addr, sizeof(addr->sin_addr));
        freeaddrinfo(result);
        
        printf("Connecting to server %s:%d (attempt %d/%d)...\n", 
               servip, port, retry_count + 1, max_retries);
        
        if(connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr))<0)
        {
            printf("Connection failed:%s \n",strerror(errno));
            close(sockfd);
            sockfd = -1;
            retry_count++;
            if(retry_count < max_retries)
            {
                printf("Retrying in %d seconds..\n",retry_interval);
                sleep(retry_interval);
            }
        }
    }

    if(sockfd >= 0)
    {
        printf("Connected to server successfully!\n");
    }
    else
    {
        printf("Failed to connect after %d attempts\n", max_retries);
    }

    return sockfd;
}

int send_cached_data(int sockfd)
{
    const char *sql="SELECT id, device_id, time, temp FROM temperature_cache ORDER BY created_at ASC;";
    sqlite3_stmt *stmt;
    int      sent_count = 0;

    if(sqlite3_prepare_v2(cache_db,sql,-1,&stmt,NULL)!=SQLITE_OK)
    {
        printf("Failed to prepare cache query: %s\n", sqlite3_errmsg(cache_db));
        return -1;
    }
    
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
        int id = sqlite3_column_int(stmt, 0);
        const char *device_id = (const char*)sqlite3_column_text(stmt, 1);
        const char *time_str = (const char*)sqlite3_column_text(stmt, 2);
        float temp = sqlite3_column_double(stmt, 3);
        
        char report[100];
        snprintf(report, sizeof(report), "%s,%s,%.2f", device_id, time_str, temp);

        int len =strlen(report);
        int result=write(sockfd,report,len);

        if(result==len)
        {
            printf("Successfully sent cached record %d: %s\n", id, report);

            char delete_sql[128];
            snprintf(delete_sql, sizeof(delete_sql),
                    "DELETE FROM temperature_cache WHERE id = %d;", id);
            sqlite3_exec(cache_db, delete_sql, NULL, 0, NULL);

            sent_count++;
        }
        else
        {
            printf("Failed to send cached record,connection may be broken\n");
            break;
        }
    }

    sqlite3_finalize(stmt);
    printf("Sent %d cached records\n", sent_count);
    return sent_count;
}

void print_usage(char *progname)
{
    printf("%s usage: \n",progname);
    printf(" -I(--interval): Time interval. \n");
    printf(" -i(--ipaddr): server IP address. \n");
    printf(" -p(--port): server port. \n");
    printf(" -r(--reconnect-interval): Reconnect interval in seconds. \n");
    printf(" -R(--max-retries): Maximum reconnection attempts. \n");
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
    return rv;
}

int get_devid(char *id,int len);

int main(int argc,char **argv)
{
    int sockfd=-1;
    int rv=-1;
    char *servip="127.0.0.1";
    int port=6666;
    struct sockaddr_in serv_addr;
    float temp;
    int interval=10;
    int reconnect_interval=5;     
    int max_reconnect_attempts=10; 

    char report[100];
    int ch;
    struct option opts[]={
        {"interval", required_argument, NULL, 'I'},
        {"ipaddr", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"reconnect-interval", required_argument, NULL, 'r'},
        {"max-retries", required_argument, NULL, 'R'},
        {"help", no_argument, NULL, 'h'},
        {0,0,0,0}
    };
    char device_id[20];
    char current_time[30];
    struct addrinfo hints, *result;

    while((ch=getopt_long(argc,argv,"I:i:p:r:R:h",opts,NULL))!=-1)
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
            case 'r':
                reconnect_interval=atoi(optarg);
                break;
            case 'R':
                max_reconnect_attempts=atoi(optarg);
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

    
    if(init_cache_database()!=0)
    {
        printf("Failed to initialize cache database,exiting..\n");
        return 1;
    }

    int connected=0;
    get_devid(device_id, 20);
    printf("Device ID: %s\n", device_id);
    printf("Reconnect interval: %d seconds\n", reconnect_interval);
    printf("Max reconnect attempts: %d\n", max_reconnect_attempts);

    while(1)
    {
        if(ds18b20_get_temperature(&temp)<0)
        {
            printf("Get ds18b20 temperature failure !\n");
            sleep(interval);
            continue;
        }

        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", t);

        snprintf(report, sizeof(report), "%s,%s,%.2f", device_id, current_time, temp);

        if(sockfd < 0 || !connected)
        {
            printf("Not connected to server, attempting to connect...\n");
            sockfd= connect_to_server(servip,port, max_reconnect_attempts, reconnect_interval);

            if(sockfd >= 0)
            {
                connected=1;
                printf("Connection established\n");

                send_cached_data(sockfd);
            }
            else
            {
                connected = 0;
                printf("Connection failed, saving data to cache\n");
            }
        }

        if(connected)
        {
            int len = strlen(report);
            int result = write(sockfd, report, len);
            
            if(result == len)
            {
                printf("Temperature %.2f℃ sent successfully\n", temp);
            }
            else
            {
                printf("Failed to send data: %s\n", strerror(errno));
                printf("Saving to cache and closing connection...\n");
                
                save_to_cache(device_id, current_time, temp);
                
                close(sockfd);
                sockfd = -1;
                connected = 0;
            }
        }
        else
        {
            printf("Offline mode: Saving temperature %.2f℃ to cache\n", temp);
            save_to_cache(device_id, current_time, temp);
        }
        sleep(interval);
    }
    
    
    if(sockfd >= 0) close(sockfd);
    if(cache_db != NULL) sqlite3_close(cache_db);
    return 0;
}

int get_devid(char *id,int len)
{
    int sn=1;
    snprintf(id,len,"RPI%03d",sn);
    return 0;
}
