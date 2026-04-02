#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/epoll.h>
// #include <sqlite3.h> 
#include <mysql/mysql.h>
#include <cjson/cJSON.h>

int         g_stop = 0;
#define BACKLOG             10
#define MAX_EVENTS          4096

//sqlite3 *db = NULL;
MYSQL *g_db = NULL;

#define DB_HOST     "127.0.0.1"
#define DB_PORT     3306
#define DB_USER     "test"
#define DB_PASS     "123456"
#define DB_NAME     "myproject"

int init_mysql(void)
{
    g_db = mysql_init(NULL);
    if (!g_db)
    {
        printf("mysql_init failed\n");
        return -1;
    }

    if (!mysql_real_connect(g_db, DB_HOST, DB_USER, DB_PASS, DB_NAME, DB_PORT, NULL, 0))
    {
        printf("mysql connect failed: %s\n", mysql_error(g_db));
        return -2;
    }

    printf("MySQL connected successfully!\n");

    char *sql =
        "CREATE TABLE IF NOT EXISTS temperature ("
        "id INT AUTO_INCREMENT PRIMARY KEY,"
        "device_id VARCHAR(64) NOT NULL,"
        "time VARCHAR(64) NOT NULL,"
        "temp FLOAT NOT NULL"
        ");";

    if (mysql_query(g_db, sql))
    {
        printf("create table failed: %s\n", mysql_error(g_db));
        return -3;
    }

    printf("Table ready\n");
    return 0;
}


void save_to_db(char *data)
{
    if (g_db == NULL) 
    {
        printf("ERROR: Database not initialized\n");
        return;
    }
    
    char device_id[32], time_str[32];
    float temp;
    char sql[512];
    char *err_msg = NULL;
    
    char *p = data;
    while (*p) 
    {
        if (*p == '\r' || *p == '\n') 
	{
            *p = '\0';
            break;
        }
        p++;
    }
    //Cjson analizy json
    cJSON *root = cJSON_Parse(data);
    if(root == NULL)
    {
	    printf("JSON parse error!\n");
	    return ;
    }

    cJSON *id = cJSON_GetObjectItem(root, "device_id");
    cJSON *t  = cJSON_GetObjectItem(root, "time");
    cJSON *tp = cJSON_GetObjectItem(root, "temp");

    if (!id || !t || !tp) 
    {
        printf("JSON missing fields\n");
        cJSON_Delete(root);
        return;
    }

    snprintf(device_id, sizeof(device_id), "%s", id->valuestring);
    snprintf(time_str, sizeof(time_str), "%s", t->valuestring);
    temp = tp->valuedouble;

    cJSON_Delete(root);
    
    printf("JSON解析成功: device=%s, time=%s, temp=%.2f\n",
           device_id, time_str, temp);
    
  
   /* int result = sscanf(data, "%31[^,],%31[^,],%f", device_id, time_str, &temp);
    if (result != 3) 
    {
        printf("data error: %s (parsed %d items)\n", data, result);
        return;
    }
    
    printf("data Analysis success: device=%s, time=%s, temperature=%.2f ℃\n", 
           device_id, time_str, temp);
    */

    
    snprintf(sql, sizeof(sql),
        "INSERT INTO temperature (device_id, time, temp) VALUES ('%s', '%s', %.2f)",
        device_id, time_str, temp);
    
    if (mysql_query(g_db, sql))
    {
        printf("insert failed: %s\n", mysql_error(g_db));
    }
    else
    {
        printf("Data saved to MySQL success!\n");
    }
    
}

void sig_handler(int signum)
{
     	printf("\nReceived signal %d, shutting down server...\n", signum);
	g_stop = 1;
}

int socket_init(int port)
{
    struct sockaddr_in serv_addr;
    int listenfd;
    int reuse = 1;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0)
    {
        printf("create socket failure: %s\n", strerror(errno));
        return -1;
    }

    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        printf("bind socket failure: %s\n", strerror(errno));
        return -2;
    }

    printf("socket[%d] bind on port[%d] for all IP address ok\n", listenfd, port);

    listen(listenfd, BACKLOG);
    
    return listenfd;
}

void print_usage(char *progname)
{
    printf("%s usage: \n", progname);
    printf("  -p(--port): specify listen port.\n");
    printf("  -h(--Help): print this help information.\n");
    return;
}

int main(int argc, char **argv)
{
    int clifd, listenfd;
    int port = 6666;
   // int debug = 0;
    
    struct sockaddr_in cli_addr;
    socklen_t addr_len = sizeof(cli_addr);
    char buf[1024];
    
    int epfd;
    int nfds = 0;
    struct epoll_event ev, events[MAX_EVENTS];
    
    int fd;
    int rv = -1;
    
    int i;
    int ch;
    
    struct option opts[] = {
       // {"debug", no_argument, NULL, 'd'},
        {"port", required_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while((ch = getopt_long(argc, argv, "p:h", opts, NULL)) != -1)
    {
        switch(ch)
        {
           // case 'd':
              //  debug = 1;
              //  break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
        }
    }
    
    //if(!debug)
      //  daemon(0, 0);

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGSEGV, sig_handler);
    signal(SIGPIPE, sig_handler);

    printf("Initializing MySQL...\n");
    if(init_mysql() != 0)
    {
        printf("Failed to initialize MySQL\n");
        return 1;
    }
    printf("Database initialized successfully\n");

    if((listenfd = socket_init(port)) < 0)
        return 1;
    
    epfd = epoll_create1(0);
    if(epfd < 0)
    {
        printf("epoll_create1 failure: %s\n", strerror(errno));
        return 1;
    }

    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0)
    {
        printf("epoll_ctrl add listen_fd failure: %s\n", strerror(errno));
        return -1;
    }
    
    printf("Server started on port %d, waiting for connections...\n", port);
    
    while(!g_stop)
    {
        nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if(nfds <= 0)
        {
           	if(errno == EINTR) 
		{
            		// 被信号中断，正常退出循环，不打印错误
            		continue;
        	}
	       	printf("poll failure: %s\n", strerror(errno));
		continue;
        }

        for(i = 0; i < nfds; i++)
        {
            fd = events[i].data.fd;
            
            if(fd == listenfd)
            {
                clifd = accept(listenfd, (struct sockaddr*)&cli_addr, &addr_len);
                if(clifd < 0)
                {
                    printf("accept new socket failure: %s\n", strerror(errno));
                    continue;
                }

                printf("Accept new client[%s:%d] with fd [%d]\n", 
                       inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port), clifd);
                
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = clifd;
                if(epoll_ctl(epfd, EPOLL_CTL_ADD, clifd, &ev) < 0)
                {
                    perror("epoll_ctl add client");
                    close(clifd);
                }
            }
            else if(events[i].events & EPOLLIN)
            {
                memset(buf, 0, sizeof(buf));
                rv = read(fd, buf, sizeof(buf) - 1);
                if(rv <= 0)
                {
                    printf("socket[%d] get disconnect\n", fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }
                
                // 确保字符串以null结尾
                buf[rv] = '\0';
                printf("Received data: %s\n", buf);
                
                save_to_db(buf);
            }

            if(events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
            {
                printf("socket[%d] closed or error\n", fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }
        }
    }
    
    close(epfd);
    
    if(g_db != NULL) 
    {
        mysql_close(g_db);
    }
    
    return 0;
}
