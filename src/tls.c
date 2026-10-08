nclude <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "log.h"
#include "tls.h"
#include "util.h"

static int
always_true_callback(X509_STORE_CTX *ctx, void *arg)
{
	(void)(ctx);
	(void)(arg);
	return 1;
}

static int
tls_host_gencert(struct gmnisrv_tls *tlsconf, struct gmnisrv_host *host,
	const char *crtpath, const char *keypath)
{
	server_log("generating certificate for %s", host->hostname);

	EVP_PKEY *pkey = EVP_PKEY_new();
	if (!pkey) {
		server_error("EVP_PKEY_new failed");
		return 1;
	}

	EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_secp384r1);
	if (!ec_key) {
		server_error("EC_KEY_new_by_curve_name failed");
		EVP_PKEY_free(pkey);
		return 1;
	}
	if (EC_KEY_generate_key(ec_key) != 1) {
		server_error("EC_KEY_generate_key failed");
		EC_KEY_free(ec_key);
		EVP_PKEY_free(pkey);
		return 1;
	}

	EVP_PKEY_assign_EC_KEY(pkey, ec_key);

	X509 *x509 = X509_new();
	if (!x509) {
		server_error("X509_new failed");
		EVP_PKEY_free(pkey);
		return 1;
	}

	X509_set_version(x509, 2);

	/* Random serial number, per RFC 5280 */
	BIGNUM *serial = BN_new();
	if (!serial || !BN_rand(serial, 159, BN_RAND_TOP_ANY,
			BN_RAND_BOTTOM_ANY)) {
		server_error("Failed to generate certificate serial");
		BN_free(serial);
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}
	BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(x509));
	BN_free(serial);

	X509_gmtime_adj(X509_get_notBefore(x509), 0);
	X509_gmtime_adj(X509_get_notAfter(x509), INT32_MAX); // 68 years
	X509_set_pubkey(x509, pkey);

	const char *organization = "gmnisrv";
	if (tlsconf->organization != NULL) {
		organization = tlsconf->organization;
	}
	X509_NAME *name = X509_get_subject_name(x509);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
		(unsigned char *)organization, -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
		(unsigned char *)host->hostname, -1, -1, 0);
	X509_set_issuer_name(x509, name);

	X509V3_CTX ctx;
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, NULL, x509, NULL, NULL, 0);
	char alt_name[512];
	int r = snprintf(alt_name, sizeof(alt_name), "DNS:%s", host->hostname);
	if (r < 0 || (size_t)r >= sizeof(alt_name)) {
		server_error("hostname too long for SAN: %s", host->hostname);
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}
	X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx,
		NID_subject_alt_name, alt_name);
	if (!ext) {
		server_error("Failed to build SAN extension");
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}
	X509_add_ext(x509, ext, -1);
	X509_EXTENSION_free(ext);

	if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
		server_error("X509_sign failed");
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}

	int pfd = open(keypath, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (pfd < 0) {
		server_error("opening private key for writing failed: %s",
			strerror(errno));
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}

	FILE *pf = fdopen(pfd, "w");
	if (!pf) {
		server_error("fdopen failed: %s", strerror(errno));
		close(pfd);
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}

	r = PEM_write_PrivateKey(pf, pkey, NULL, NULL, 0, NULL, NULL);
	fclose(pf);
	if (r != 1) {
		server_error("writing private key failed");
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}

	FILE *xf = fopen(crtpath, "w");
	if (!xf) {
		server_error("opening certificate for writing failed: %s",
			strerror(errno));
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}
	r = PEM_write_X509(xf, x509);
	fclose(xf);
	if (r != 1) {
		server_error("writing certificate failed");
		X509_free(x509);
		EVP_PKEY_free(pkey);
		return 1;
	}

	host->x509 = x509;
	host->pkey = pkey;
	return 0;
}

static int
tls_host_init(struct gmnisrv_tls *tlsconf, struct gmnisrv_host *host)
{
	char crtpath[PATH_MAX + 1];
	char keypath[PATH_MAX + 1];
	int n = snprintf(crtpath, sizeof(crtp
