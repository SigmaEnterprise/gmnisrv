nclude <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "ini.h"
#include "regexp.h"

struct gmnisrv_host *
gmnisrv_config_get_host(struct gmnisrv_config *conf, const char *hostname)
{
	struct gmnisrv_host *host = conf->hosts;
	while (host) {
		if (strcasecmp(host->hostname, hostname) == 0) {
			return host;
		}
		host = host->next;
	}
	return NULL;
}

struct gmnisrv_route *
gmnisrv_host_get_route(struct gmnisrv_host *host,
		enum gmnisrv_routing routing, const char *spec)
{
	struct gmnisrv_route *route = host->routes;
	while (route) {
		if (route->routing == routing
				&& strcmp(route->spec, spec) == 0) {
			return route;
		}
		route = route->next;
	}
	return NULL;
}

static int
parse_listen(struct gmnisrv_config *conf, const char *value)
{
	char listen[4096];
	if (strlen(value) >= sizeof(listen) - 1) {
		fprintf(stderr, "Error: listen directive too long\n");
		return 0;
	}
	strcpy(listen, value);

	char *tok = strtok(listen, " ");
	while (tok) {
		char *port = tok;
		struct gmnisrv_bind *bind =
			calloc(1, sizeof(struct gmnisrv_bind));
		if (!bind) {
			fprintf(stderr, "Error: out of memory\n");
			return 0;
		}
		bind->port = 1965;
		bind->name = strdup(tok);

		if (tok[0] == '[') {
			bind->family = AF_INET6;
			char *e = strchr(tok, ']');
			if (!e) {
				fprintf(stderr,
					"Error: invalid address specification %s\n",
					tok);
				free(bind->name);
				free(bind);
				return 0;
			}
			*e = '\0';
			port = e + 1;
			++tok;
		} else {
			bind->family = AF_INET;
		}

		port = strchr(port, ':');
		if (port) {
			*port = '\0';
			++port;

			char *endptr;
			bind->port = strtoul(port, &endptr, 10);
			if (*endptr) {
				fprintf(stderr, "Error: invalid port %s\n", port);
				free(bind->name);
				free(bind);
				return 0;
			}
			if (bind->port == 0 || bind->port > 65535) {
				fprintf(stderr, "Error: invalid port %s\n", port);
				free(bind->name);
				free(bind);
				return 0;
			}
		}

		int r = inet_pton(bind->family, tok, bind->addr);
		if (r != 1) {
			fprintf(stderr,
				"Error: invalid address specification %s\n",
				tok);
			free(bind->name);
			free(bind);
			return 0;
		}

		bind->next = conf->binds;
		conf->binds = bind;
		tok = strtok(NULL, " ");
	}

	return 1;
}

static int
conf_ini_handler(void *user, const char *section,
	const char *name, const char *value)
{
	struct gmnisrv_config *conf = (struct gmnisrv_config *)user;
	struct {
		char *section;
		char *name;
		char **value;
	} strvars[] = {
		{ ":tls", "store", &conf->tls.store },
		{ ":tls", "organization", &conf->tls.organization },
	};
	struct {
		char *section;
		char *name;
		int (*fn)(struct gmnisrv_config *conf, const char *value);
	} fnvars[] = {
		{ "", "listen", &parse_listen, }
	};

	for (size_t i = 0; i < sizeof(strvars) / sizeof(strvars[0]); ++i) {
		if (strcmp(strvars[i].section, section) != 0) {
			continue;
		}
		if (strcmp(strvars[i].name, name) != 0) {
			continue;
		}
		free(*strvars[i].value);
		*strvars[i].value = strdup(value);
		return 1;
	}

	for (size_t i = 0; i < sizeof(fnvars) / sizeof(fnvars[0]); ++i) {
		if (strcmp(fnvars[i].section, section) != 0) {
			continue;
		}
		if (strcmp(fnvars[i].name, name) != 0) {
			continue;
		}
		return fnvars[i].fn(conf, value);
	}

	if (section[0] == '\0' || section[0] == ':') {
		fprintf(stderr, "Unknown config option [%s]%s\n", section, name);
		return 0;
	}

	const char *spec;
	char hostname[1024 + 1];
	enum gmnisrv_routing routing;
	size_t hostln = strcspn(section, "=:~");
	switch (section[hostln]) {
	case '\0':
		routing = ROUTE_PATH;
		spec = "/";
		break;
	case '=':
		routing = ROUTE_EXACT;
		spec = &section[hostln + 1];
		break;
	case ':':
		routing = ROUTE_PATH;
		spec = &section[hostln + 1];
		break;
	case '~':
		routing = ROUTE_REGEX;
		spec = &section[hostln + 1];
		break;
	default:
		assert(0);
	}
	if (hostln >= sizeof(hostname)) {
		fprintf(stderr, "Error: hostname too long: %s\n", section);
		return 0;
	}
	strncpy(hostname, section, hostln);
	hostname[hostln]
