#pragma once
#include <syslog.h>

#define _LOG(level, fmt, ...) syslog(level, "[egepod] " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...)  _LOG(LOG_ERR,     "ERROR " fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...)  _LOG(LOG_WARNING, "WARN  " fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...)  _LOG(LOG_INFO,    "INFO  " fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...)  _LOG(LOG_DEBUG,   "DEBUG " fmt, ##__VA_ARGS__)

#define LOG_OPEN(ident)  openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON)
#define LOG_CLOSE()      closelog()
