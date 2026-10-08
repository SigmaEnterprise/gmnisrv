fndef GMNISRV_TLS
#define GMNISRV_TLS

#include <openssl/ssl.h>

struct gmnisrv_config;
struct gmnisrv_host;

int tls_init(struct gmnisrv_config *conf);
void tls_finish(struct gmnisrv_config *conf);
SSL *tls_get_ssl(struct gmnisrv_config *conf);
void tls_set_host(SSL *ssl, struct gmnisrv_host *host);

#endif
