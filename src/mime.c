nclude <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mime.h"

struct mime_info {
	char *mimetype;
	char *extension;
};

size_t mimedb_ln;
struct mime_info *mimedb;

static int
mimedb_compar(const void *va, const void *vb)
{
	const struct mime_info *a = va;
	const struct mime_info *b = vb;
	return strcmp(a->extension, b->extension);
}

static char *
lowercase_dup(const char *s)
{
	char *r = strdup(s);
	if (!r) {
		return NULL;
	}
	for (char *p = r; *p; ++p) {
		*p = tolower((unsigned char)*p);
	}
	return r;
}

void
mime_init(void)
{
	size_t mimedb_sz = 4096;
	mimedb = malloc(mimedb_sz * sizeof(struct mime_info));
	if (!mimedb) {
		fprintf(stderr, "Out of memory loading MIME database\n");
		abort();
	}

	FILE *f = fopen(MIMEDB, "r");
	if (!f) {
		fprintf(stderr, "Unable to open MIME database for reading: %s\n",
			strerror(errno));
		fprintf(stderr, "Is " MIMEDB " installed?\n");
		abort();
	}

	char *line = NULL;
	size_t n = 0;
	while (getline(&line, &n, f) != -1) {
		if (line[0] == '#') {
			continue;
		}
		size_t l = strlen(line);
		if (l > 1 && line[l - 1] == '\n') {
			line[l - 1] = '\0';
		}

		char *save = NULL;
		char *mime = strtok_r(line, " \t", &save);
		if (!mime) {
			continue;
		}
		char *ext = strtok_r(NULL, " \t", &save);
		if (!ext) {
			continue;
		}

		char *mime_copy = strdup(mime);
		if (!mime_copy) {
			fprintf(stderr, "Out of memory loading MIME database\n");
			abort();
		}

		do {
			if (mimedb_ln >= mimedb_sz) {
				mimedb_sz *= 2;
				struct mime_info *new = realloc(mimedb,
						mimedb_sz * sizeof(struct mime_info));
				if (!new) {
					fprintf(stderr, "Out of memory loading MIME database\n");
					abort();
				}
				mimedb = new;
			}

			mimedb[mimedb_ln].mimetype = strdup(mime_copy);
			mimedb[mimedb_ln].extension = lowercase_dup(ext);
			if (!mimedb[mimedb_ln].mimetype ||
					!mimedb[mimedb_ln].extension) {
				fprintf(stderr, "Out of memory loading MIME database\n");
				abort();
			}
			++mimedb_ln;
		} while ((ext = strtok_r(NULL, " \t", &save)));

		free(mime_copy);
	}
	free(line);
	fclose(f);

	qsort(mimedb, mimedb_ln, sizeof(mimedb[0]), mimedb_compar);
}

void
mime_finish(void)
{
	for (size_t i = 0; i < mimedb_ln; ++i) {
		free(mimedb[i].mimetype);
		free(mimedb[i].extension);
	}
	free(mimedb);
	mimedb = NULL;
	mimedb_ln = 0;
}

static bool
has_suffix(const char *str, const char *suff)
{
	size_t a = strlen(str), b = strlen(suff);
	if (a < b) {
		return false;
	}
	return strcasecmp(&str[a - b], suff) == 0;
}

static int
path_compar(const void *va, const void *vb)
{
	const char *ext = va;
	const struct mime_info *mime = vb;
	return strcmp(ext, mime->extension);
}

const char *
mimetype_for_path(const char *path)
{
	if (has_suffix(path, ".gmi") || has_suffix(path, ".gemini")) {
		return "text/gemini";
	}

	const char *ext = strrchr(path, '.');
	if (!ext || !ext[1]) {
		return "application/octet-stream";
	}
	++ext;

	char *ext_lower = lowercase_dup(ext);
	if (!ext_lower) {
		return "application/octet-stream";
	}

	struct mime_info *mime = bsearch(
		ext_lower, mimedb, mimedb_ln, sizeof(mimedb[0]), path_compar);
	free(ext_lower);
	if (!mime) {
		return "application/octet-stream";
	}
	return mime->mimetype;
}
