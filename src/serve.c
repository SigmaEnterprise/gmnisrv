nclude <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "config.h"
#include "gemini.h"
#include "log.h"
#include "mime.h"
#include "server.h"
#include "url.h"

void
client_submit_response(struct gmnisrv_client *client,
	enum gemini_status status, const char *meta, FILE *body)
{
	/* Reject CR/LF in meta to prevent header injection */
	for (const char *p = meta; *p; ++p) {
		if (*p == '\r' || *p == '\n') {
			client->state = CLIENT_STATE_HEADER;
			client->status = GEMINI_STATUS_PERMANENT_FAILURE;
			client->meta = strdup("Invalid meta");
			client->body = NULL;
			client->pollfd->events = POLLOUT;
			return;
		}
	}
	client->state = CLIENT_STATE_HEADER;
	client->status = status;
	client->meta = strdup(meta);
	client->body = body;
	client->pollfd->events = POLLOUT;
}

void
client_oom(struct gmnisrv_client *client)
{
	const char *error = "Out of memory";
	client_submit_response(client,
		GEMINI_STATUS_TEMPORARY_FAILURE, error, NULL);
}

static int
namecmp(const void *p1, const void *p2)
{
	return strcmp(*(char *const *)p1, *(char *const *)p2);
}

static void
serve_autoindex(struct gmnisrv_client *client, const char *path)
{
	size_t bufsz = 0;
	size_t nameln = 0, namesz = 1024;
	char **names = calloc(namesz, sizeof(char *));
	if (!names) {
		client_oom(client);
		return;
	}

	DIR *dirp = opendir(path);
	if (!dirp) {
		goto internal_error;
	}

	bufsz += snprintf(NULL, 0, "# Index of %s\n\n", client->path);

	struct dirent *ent;
	errno = 0;
	while ((ent = readdir(dirp)) != NULL) {
		char fpath[PATH_MAX + 1];
		snprintf(fpath, sizeof(fpath), "%s/%s", path, ent->d_name);

		struct stat st;
		if (stat(fpath, &st) != 0) {
			goto internal_error;
		}

		if ((S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || S_ISDIR(st.st_mode))
				&& ent->d_name[0] != '.') {
			if (nameln >= namesz) {
				char **new = realloc(names, namesz * 2 * sizeof(char *));
				if (!new) {
					goto internal_error;
				}
				namesz *= 2;
				names = new;
			}
			if (S_ISDIR(st.st_mode)){
				names[nameln] = malloc(strlen(ent->d_name)+2);
				if (!names[nameln]) {
					goto internal_error;
				}
				sprintf(names[nameln], "%s/", ent->d_name);
			} else {
				names[nameln] = strdup(ent->d_name);
				if (!names[nameln]) {
					goto internal_error;
				}
			}
			bufsz += snprintf(NULL, 0, "=> %s\n", names[nameln]);
			nameln++;
		}

		errno = 0;
	}
	if (errno != 0) {
		goto internal_error;
	}

	qsort(names, nameln, sizeof(names[0]), namecmp);

	bufsz++; // buffer needs to have room for the \0.
	FILE *buf = fmemopen(NULL, bufsz, "w+");
	if (!buf) {
		goto internal_error;
	}
	int r;
	r = fprintf(buf, "# Index of %s\n\n", client->path);
	if (r <= 0) {
		fclose(buf);
		goto internal_error;
	}
	for (size_t i = 0; i < nameln; ++i) {
		r = fprintf(buf, "=> %s\n", names[i]);
		if (r <= 0) {
			fclose(buf);
			goto internal_error;
		}
	}
	if (fseek(buf, 0, SEEK_SET) != 0) {
		fclose(buf);
		goto internal_error;
	}
	client_submit_response(client, GEMINI_STATUS_SUCCESS,
		"text/gemini", buf);

exit:
	if (dirp) closedir(dirp);
	for (size_t i = 0; i < nameln; ++i) {
		free(names[i]);
	}
	free(names);
	return;

internal_error:
	server_error("Error reading %s: %s", path, strerror(errno));
	client_submit_response(client, GEMINI_STATUS_PERMANENT_FAILURE,
		"Internal server error", NULL);
	goto exit;
}

static void
serve_cgi(struct gmnisrv_client *client, const char *path,
		const char *script_name, const char *pathinfo)
{
	int pfd[2];
	if (pipe(pfd) == -1) {
		server_error("pipe: %s", strerror(errno));
		client_submit_response(client, GEMINI_STATUS_PERMANENT_FAILURE,
			"Internal server error", NULL);
		return;
	}

	signal(SIGCHLD, SIG_IGN);
	pid_t pid = fork();
	if (pid == -1) {
		server_error("fork: %s", strerror(errno));
		client_submit_response(client, GEMINI_STATUS_PERMANENT_FAILURE,
			"Internal server error", NULL);
		close
