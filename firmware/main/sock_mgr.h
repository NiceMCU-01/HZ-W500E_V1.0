#ifndef SOCK_MGR_H
#define SOCK_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

typedef enum {
    SOCK_NONE = 0,
    SOCK_TCPC,
    SOCK_TCPS,
    SOCK_UDPC,
    SOCK_UDPS,
    SOCK_HTPC
} sock_type_t;

/* 接收数据回调（由 AT 层注册，socket 收到数据时调用） */
typedef void (*sock_rx_sink_t)(const uint8_t *d, int len);

void        sock_set_sink(sock_rx_sink_t fn);
esp_err_t   sock_init(void);

/* type 决定行为；ip/rport/lport 依 type 语义 */
esp_err_t sock_open(sock_type_t type, const char *ip, int rport, int lport);

/* HTTP GET，url 形如 http://host:port/path */
esp_err_t sock_http_get(const char *url);

esp_err_t sock_send(const uint8_t *d, int len);
void     sock_close(void);
bool     sock_is_open(void);
sock_type_t sock_get_type(void);

/* 透传模式 */
void     sock_set_transparent(int on);
int      sock_is_transparent(void);

/* 最近一次 connect()/socket()/bind() 等失败的诊断信息。
   AT 层把它以 "+CIPERR,<stage>,<errno>" 形式输出——
   AT 固件不能开日志（日志会混进 AT 应答），只能靠应答本身带诊断码。 */
typedef struct {
    int stage;      /* 0=无 1=socket 2=connect 3=bind 4=listen 5=send */
    int err;        /* errno 数值 */
    int last_ok;    /* 对应的阶段序号，用于查询 */
} sock_diag_t;

const sock_diag_t *sock_diag(void);

#endif /* SOCK_MGR_H */