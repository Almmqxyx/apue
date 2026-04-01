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
#include <signal.h>

#include "logger.h"

sqlite3     *cache_db = NULL;
#define HEARTBEAT_INTERVAL 3

void send_heartbeat(int sockfd)
{
    if (sockfd < 0) return;

    const char *heartbeat = "{}";
    int ret = write(sockfd, heartbeat, strlen(heartbeat));

    if (ret < 0)
    {
        log_error("Heartbeat failed, connection lost\n");
        close(sockfd);
        sockfd = -1;
    }
}

int init_cache_database(void)
{
    int rc;
    char    *err_msg = NULL;

    rc = sqlite3_open("temperature_cache.db", &cache_db);
    if (rc != SQLITE_OK)
    {
        log_error("Failed to open cache database: %s\n", sqlite3_errmsg(cache_db));
        return -1;
    }

    char *sql =
        "CREATE TABLE IF NOT EXISTS temperature_cache ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "device_id TEXT NOT NULL,"
        "time TEXT NOT NULL,"
        "temp REAL NOT NULL,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP);";
    rc = sqlite3_exec(cache_db, sql, NULL, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        log_error("Failed to create cache table: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(cache_db);
        return -2;
    }

    log_info("Cache database initialized successfully\n");
    return 0;
}

int save_to_cache(const char *device_id, const char *time_str, float temp)
{
    char        sql[512];
    char        *err_msg = NULL;
    int     rc;

    snprintf(sql, sizeof(sql),
            "INSERT INTO temperature_cache(device_id,time,temp) "
            "VALUES ('%s','%s',%.2f);",
            device_id, time_str, temp);
    rc = sqlite3_exec(cache_db, sql, NULL, 0, &err_msg);
    if (rc != SQLITE_OK)
    {
        log_error("Failed to save to cache: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    log_info("Saved to cache: device=%s, temp=%.2f\n", device_id, temp);
    return 0;
}

int connect_to_server(const char *servip, int port, int max_retries, int retry_interval)
{
    int sockfd = -1;
    int retry_count = 0;
    struct sockaddr_in serv_addr;
    struct addrinfo hints, *result;

    while (retry_count < max_retries && sockfd < 0)
    {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            log_error("Failed to create socket: %s\n", strerror(errno));
            retry_count++;
            goto retry_wait;
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int gai_ret = getaddrinfo(servip, NULL, &hints, &result);
        if (gai_ret != 0)
        {
            log_error("DNS resolution failure: %s\n", gai_strerror(gai_ret));
            close(sockfd);
            sockfd = -1;
            retry_count++;
            goto retry_wait;
        }

        struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
        memcpy(&serv_addr.sin_addr, &addr->sin_addr, sizeof(addr->sin_addr));
        freeaddrinfo(result);

        log_info("Connecting to %s:%d (attempt %d/%d)\n", servip, port, retry_count + 1, max_retries);

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        {
            log_error("Connection failed: %s\n", strerror(errno));
            close(sockfd);
            sockfd = -1;
            retry_count++;
            goto retry_wait;
        }

        log_info("Connected to server successfully\n");
        break;

retry_wait:
        if (retry_count < max_retries)
        {
            log_warn("Retry after %d seconds\n", retry_interval);
            sleep(retry_interval);
        }
    }

    if (sockfd < 0)
    {
        log_error("Failed to connect after %d attempts\n", max_retries);
    }

    return sockfd;
}

int send_cached_data(int sockfd)
{
    const char *sql = "SELECT id, device_id, time, temp FROM temperature_cache ORDER BY created_at ASC;";
    sqlite3_stmt *stmt;
    int      sent_count = 0;

    if (sqlite3_prepare_v2(cache_db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        log_error("Failed to prepare cache query: %s\n", sqlite3_errmsg(cache_db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int id = sqlite3_column_int(stmt, 0);
        const char *device_id = (const char*)sqlite3_column_text(stmt, 1);
        const char *time_str = (const char*)sqlite3_column_text(stmt, 2);
        float temp = sqlite3_column_double(stmt, 3);

        char report[128];
        snprintf(report, sizeof(report), "{\"device_id\":\"%s\",\"time\":\"%s\",\"temp\":%.2f}", device_id, time_str, temp);

        int len = strlen(report);
        int result = write(sockfd, report, len);

        if (result == len)
        {
            log_info("Sent cached record %d: %s\n", id, report);
            char delete_sql[128];
            snprintf(delete_sql, sizeof(delete_sql), "DELETE FROM temperature_cache WHERE id = %d;", id);
            sqlite3_exec(cache_db, delete_sql, NULL, 0, NULL);
            sent_count++;
        }
        else
        {
            log_error("Failed to send cached record\n");
            break;
        }
    }

    sqlite3_finalize(stmt);
    log_info("Sent %d cached records\n", sent_count);
    return sent_count;
}

void print_usage(char *progname)
{
    printf("%s usage:\n", progname);
    printf(" -I(--interval): Time interval.\n");
    printf(" -i(--ipaddr): server IP address.\n");
    printf(" -p(--port): server port.\n");
    printf(" -r(--reconnect-interval): Reconnect interval in seconds.\n");
    printf(" -R(--max-retries): Maximum reconnection attempts.\n");
    printf(" -h(--Help): print this help information.\n");
}

int ds18b20_get_temperature(float *temp)
{
    char            w1_path[50] = "/sys/bus/w1/devices/";
    char            chip[20];
    char            buf[128];
    DIR            *dirp;
    struct dirent  *direntp;
    int             fd = -1;
    char           *ptr;
    int             found = 0;

    if (!temp)
        return -1;

    if ((dirp = opendir(w1_path)) == NULL)
    {
        log_error("opendir failed: %s\n", strerror(errno));
        return -2;
    }

    while ((direntp = readdir(dirp)) != NULL)
    {
        if (strstr(direntp->d_name, "28-"))
        {
            strcpy(chip, direntp->d_name);
            found = 1;
            break;
        }
    }
    closedir(dirp);

    if (!found)
    {
        log_error("No DS18B20 sensor found\n");
        return -3;
    }

    snprintf(w1_path, sizeof(w1_path), "/sys/bus/w1/devices/%s/w1_slave", chip);

    if ((fd = open(w1_path, O_RDONLY)) < 0)
    {
        log_error("open %s failed: %s\n", w1_path, strerror(errno));
        return -4;
    }

    if (read(fd, buf, sizeof(buf)) < 0)
    {
        log_error("read failed: %s\n", strerror(errno));
        close(fd);
        return -5;
    }

    ptr = strstr(buf, "t=");
    if (!ptr)
    {
        log_error("Can not find temperature data\n");
        close(fd);
        return -6;
    }

    *temp = atof(ptr + 2) / 1000.0;
    close(fd);
    return 0;
}

int get_devid(char *id, int len);

int main(int argc, char **argv)
{
    int sockfd = -1;
    char *servip = "127.0.0.1";
    int port = 6666;
    float temp;
    int interval = 10;
    int reconnect_interval = 5;
    int max_reconnect_attempts = 10;
    char device_id[20];
    char current_time[30];
    int connected = 0;
    time_t last_heartbeat = 0;

    log_open("console", LOG_LEVEL_DEBUG, 0, 1);
    log_info("Temperature client started\n");

    signal(SIGPIPE, SIG_IGN);

    int ch;
    struct option opts[] = {
        {"interval", required_argument, NULL, 'I'},
        {"ipaddr", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"reconnect-interval", required_argument, NULL, 'r'},
        {"max-retries", required_argument, NULL, 'R'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    while ((ch = getopt_long(argc, argv, "I:i:p:r:R:h", opts, NULL)) != -1)
    {
        switch (ch)
        {
            case 'I':
                interval = atoi(optarg);
                break;
            case 'i':
                servip = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                reconnect_interval = atoi(optarg);
                break;
            case 'R':
                max_reconnect_attempts = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                log_close();
                return 0;
        }
    }

    get_devid(device_id, 20);
    log_info("Device ID: %s\n", device_id);

    if (init_cache_database() != 0)
    {
        log_error("Failed to initialize cache database, exiting\n");
        log_close();
        return 1;
    }

    while (1)
    {
        if (ds18b20_get_temperature(&temp) < 0)
        {
            log_error("Failed to read temperature\n");
            sleep(1);
            continue;
        }

        log_debug("Current temperature: %.2f°C\n", temp);

        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", t);

        char report[128];
        snprintf(report, sizeof(report), "{\"device_id\":\"%s\",\"time\":\"%s\",\"temp\":%.2f}", device_id, current_time, temp);

        if (sockfd < 0)
        {
            log_warn("Not connected, trying to reconnect\n");
            sockfd = connect_to_server(servip, port, max_reconnect_attempts, reconnect_interval);
            if (sockfd >= 0)
            {
                connected = 1;
                send_cached_data(sockfd);
                last_heartbeat = time(NULL);
            }
            else
            {
                connected = 0;
            }
        }

        if (connected)
        {
            errno = 0;
            int result = write(sockfd, report, strlen(report));

            if (result > 0)
            {
                log_info("Temperature sent: %.2f°C\n", temp);
            }
            else
            {
                log_error("Send failed, connection broken\n");
                save_to_cache(device_id, current_time, temp);
                close(sockfd);
                sockfd = -1;
                connected = 0;
                continue;
            }

            now = time(NULL);
            if ((now - last_heartbeat) >= HEARTBEAT_INTERVAL)
            {
                send_heartbeat(sockfd);
                last_heartbeat = now;
            }
        }
        else
        {
            log_warn("Offline mode, saving data to cache\n");
            save_to_cache(device_id, current_time, temp);
        }

        sleep(interval);
    }

    log_close();
    return 0;
}

int get_devid(char *id, int len)
{
    snprintf(id, len, "RPI001");
    return 0;
}
