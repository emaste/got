/*
 * Copyright (c) 2018, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/tree.h>

#include <dirent.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sha1.h>
#include <zlib.h>
#include <fnmatch.h>
#include <libgen.h>
#include <uuid.h>
#include <util.h>

#include "openbsd-compat.h"

#include "got_error.h"
#include "got_repository.h"
#include "got_reference.h"
#include "got_object.h"
#include "got_path.h"
#include "got_cancel.h"
#include "got_worktree.h"
#include "got_opentemp.h"
#include "got_diff.h"

#include "got_lib_worktree.h"
#include "got_lib_sha1.h"
#include "got_lib_fileindex.h"
#include "got_lib_inflate.h"
#include "got_lib_delta.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_object_create.h"
#include "got_lib_object_idset.h"
#include "got_lib_diff.h"
#include "got_lib_gotconfig.h"

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

#define GOT_MERGE_LABEL_MERGED	"merged change"
#define GOT_MERGE_LABEL_BASE	"3-way merge base"

static const struct got_error *
create_meta_file(const char *path_got, const char *name, const char *content)
{
	const struct got_error *err = NULL;
	char *path;

	if (asprintf(&path, "%s/%s", path_got, name) == -1)
		return got_error_from_errno("asprintf");

	err = got_path_create_file(path, content);
	free(path);
	return err;
}

static const struct got_error *
update_meta_file(const char *path_got, const char *name, const char *content)
{
	const struct got_error *err = NULL;
	FILE *tmpfile = NULL;
	char *tmppath = NULL;
	char *path = NULL;

	if (asprintf(&path, "%s/%s", path_got, name) == -1) {
		err = got_error_from_errno("asprintf");
		path = NULL;
		goto done;
	}

	err = got_opentemp_named(&tmppath, &tmpfile, path);
	if (err)
		goto done;

	if (content) {
		int len = fprintf(tmpfile, "%s\n", content);
		if (len != strlen(content) + 1) {
			err = got_error_from_errno2("fprintf", tmppath);
			goto done;
		}
	}

	if (rename(tmppath, path) != 0) {
		err = got_error_from_errno3("rename", tmppath, path);
		unlink(tmppath);
		goto done;
	}

done:
	if (fclose(tmpfile) != 0 && err == NULL)
		err = got_error_from_errno2("fclose", tmppath);
	free(tmppath);
	return err;
}

static const struct got_error *
read_meta_file(char **content, const char *path_got, const char *name)
{
	const struct got_error *err = NULL;
	char *path;
	int fd = -1;
	ssize_t n;
	struct stat sb;

	*content = NULL;

	if (asprintf(&path, "%s/%s", path_got, name) == -1) {
		err = got_error_from_errno("asprintf");
		path = NULL;
		goto done;
	}

	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd == -1) {
		if (errno == ENOENT)
			err = got_error_path(path, GOT_ERR_WORKTREE_META);
		else
			err = got_error_from_errno2("open", path);
		goto done;
	}
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		err = (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno2("flock", path));
		goto done;
	}

	if (fstat(fd, &sb) != 0) {
		err = got_error_from_errno2("fstat", path);
		goto done;
	}
	*content = calloc(1, sb.st_size);
	if (*content == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	n = read(fd, *content, sb.st_size);
	if (n != sb.st_size) {
		err = (n == -1 ? got_error_from_errno2("read", path) :
		    got_error_path(path, GOT_ERR_WORKTREE_META));
		goto done;
	}
	if ((*content)[sb.st_size - 1] != '\n') {
		err = got_error_path(path, GOT_ERR_WORKTREE_META);
		goto done;
	}
	(*content)[sb.st_size - 1] = '\0';

done:
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno2("close", path_got);
	free(path);
	if (err) {
		free(*content);
		*content = NULL;
	}
	return err;
}

static const struct got_error *
write_head_ref(const char *path_got, struct got_reference *head_ref)
{
	const struct got_error *err = NULL;
	char *refstr = NULL;

	if (got_ref_is_symbolic(head_ref)) {
		refstr = got_ref_to_str(head_ref);
		if (refstr == NULL)
			return got_error_from_errno("got_ref_to_str");
	} else {
		refstr = strdup(got_ref_get_name(head_ref));
		if (refstr == NULL)
			return got_error_from_errno("strdup");
	}
	err = update_meta_file(path_got, GOT_WORKTREE_HEAD_REF, refstr);
	free(refstr);
	return err;
}

const struct got_error *
got_worktree_init(const char *path, struct got_reference *head_ref,
    const char *prefix, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *commit_id = NULL;
	uuid_t uuid;
	uint32_t uuid_status;
	int obj_type;
	char *path_got = NULL;
	char *formatstr = NULL;
	char *absprefix = NULL;
	char *basestr = NULL;
	char *uuidstr = NULL;

	if (strcmp(path, got_repo_get_path(repo)) == 0) {
		err = got_error(GOT_ERR_WORKTREE_REPO);
		goto done;
	}

	err = got_ref_resolve(&commit_id, repo, head_ref);
	if (err)
		return err;
	err = got_object_get_type(&obj_type, repo, commit_id);
	if (err)
		return err;
	if (obj_type != GOT_OBJ_TYPE_COMMIT)
		return got_error(GOT_ERR_OBJ_TYPE);

	if (!got_path_is_absolute(prefix)) {
		if (asprintf(&absprefix, "/%s", prefix) == -1)
			return got_error_from_errno("asprintf");
	}

	/* Create top-level directory (may already exist). */
	if (mkdir(path, GOT_DEFAULT_DIR_MODE) == -1 && errno != EEXIST) {
		err = got_error_from_errno2("mkdir", path);
		goto done;
	}

	/* Create .got directory (may already exist). */
	if (asprintf(&path_got, "%s/%s", path, GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}
	if (mkdir(path_got, GOT_DEFAULT_DIR_MODE) == -1 && errno != EEXIST) {
		err = got_error_from_errno2("mkdir", path_got);
		goto done;
	}

	/* Create an empty lock file. */
	err = create_meta_file(path_got, GOT_WORKTREE_LOCK, NULL);
	if (err)
		goto done;

	/* Create an empty file index. */
	err = create_meta_file(path_got, GOT_WORKTREE_FILE_INDEX, NULL);
	if (err)
		goto done;

	/* Write the HEAD reference. */
	err = write_head_ref(path_got, head_ref);
	if (err)
		goto done;

	/* Record our base commit. */
	err = got_object_id_str(&basestr, commit_id);
	if (err)
		goto done;
	err = create_meta_file(path_got, GOT_WORKTREE_BASE_COMMIT, basestr);
	if (err)
		goto done;

	/* Store path to repository. */
	err = create_meta_file(path_got, GOT_WORKTREE_REPOSITORY,
	    got_repo_get_path(repo));
	if (err)
		goto done;

	/* Store in-repository path prefix. */
	err = create_meta_file(path_got, GOT_WORKTREE_PATH_PREFIX,
	    absprefix ? absprefix : prefix);
	if (err)
		goto done;

	/* Generate UUID. */
	uuid_create(&uuid, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		err = got_error_uuid(uuid_status, "uuid_create");
		goto done;
	}
	uuid_to_string(&uuid, &uuidstr, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		err = got_error_uuid(uuid_status, "uuid_to_string");
		goto done;
	}
	err = create_meta_file(path_got, GOT_WORKTREE_UUID, uuidstr);
	if (err)
		goto done;

	/* Stamp work tree with format file. */
	if (asprintf(&formatstr, "%d", GOT_WORKTREE_FORMAT_VERSION) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}
	err = create_meta_file(path_got, GOT_WORKTREE_FORMAT, formatstr);
	if (err)
		goto done;

done:
	free(commit_id);
	free(path_got);
	free(formatstr);
	free(absprefix);
	free(basestr);
	free(uuidstr);
	return err;
}

static const struct got_error *
open_worktree(struct got_worktree **worktree, const char *path)
{
	const struct got_error *err = NULL;
	char *path_got;
	char *formatstr = NULL;
	char *uuidstr = NULL;
	char *path_lock = NULL;
	char *base_commit_id_str = NULL;
	int version, fd = -1;
	const char *errstr;
	struct got_repository *repo = NULL;
	uint32_t uuid_status;

	*worktree = NULL;

	if (asprintf(&path_got, "%s/%s", path, GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error_from_errno("asprintf");
		path_got = NULL;
		goto done;
	}

	if (asprintf(&path_lock, "%s/%s", path_got, GOT_WORKTREE_LOCK) == -1) {
		err = got_error_from_errno("asprintf");
		path_lock = NULL;
		goto done;
	}

	fd = open(path_lock, O_RDWR | O_EXLOCK | O_NONBLOCK);
	if (fd == -1) {
		err = (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno2("open", path_lock));
		goto done;
	}

	err = read_meta_file(&formatstr, path_got, GOT_WORKTREE_FORMAT);
	if (err)
		goto done;

	version = strtonum(formatstr, 1, INT_MAX, &errstr);
	if (errstr) {
		err = got_error_msg(GOT_ERR_WORKTREE_META,
		    "could not parse work tree format version number");
		goto done;
	}
	if (version != GOT_WORKTREE_FORMAT_VERSION) {
		err = got_error(GOT_ERR_WORKTREE_VERS);
		goto done;
	}

	*worktree = calloc(1, sizeof(**worktree));
	if (*worktree == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}
	(*worktree)->lockfd = -1;

	(*worktree)->root_path = realpath(path, NULL);
	if ((*worktree)->root_path == NULL) {
		err = got_error_from_errno2("realpath", path);
		goto done;
	}
	err = read_meta_file(&(*worktree)->repo_path, path_got,
	    GOT_WORKTREE_REPOSITORY);
	if (err)
		goto done;

	err = read_meta_file(&(*worktree)->path_prefix, path_got,
	    GOT_WORKTREE_PATH_PREFIX);
	if (err)
		goto done;

	err = read_meta_file(&base_commit_id_str, path_got,
	    GOT_WORKTREE_BASE_COMMIT);
	if (err)
		goto done;

	err = read_meta_file(&uuidstr, path_got, GOT_WORKTREE_UUID);
	if (err)
		goto done;
	uuid_from_string(uuidstr, &(*worktree)->uuid, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		err = got_error_uuid(uuid_status, "uuid_from_string");
		goto done;
	}

	err = got_repo_open(&repo, (*worktree)->repo_path, NULL);
	if (err)
		goto done;

	err = got_object_resolve_id_str(&(*worktree)->base_commit_id, repo,
	    base_commit_id_str);
	if (err)
		goto done;

	err = read_meta_file(&(*worktree)->head_ref_name, path_got,
	    GOT_WORKTREE_HEAD_REF);
	if (err)
		goto done;

	if (asprintf(&(*worktree)->gotconfig_path, "%s/%s/%s",
	    (*worktree)->root_path,
	    GOT_WORKTREE_GOT_DIR, GOT_GOTCONFIG_FILENAME) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	err = got_gotconfig_read(&(*worktree)->gotconfig,
	    (*worktree)->gotconfig_path);
done:
	if (repo)
		got_repo_close(repo);
	free(path_got);
	free(path_lock);
	free(base_commit_id_str);
	free(uuidstr);
	free(formatstr);
	if (err) {
		if (fd != -1)
			close(fd);
		if (*worktree != NULL)
			got_worktree_close(*worktree);
		*worktree = NULL;
	} else
		(*worktree)->lockfd = fd;

	return err;
}

const struct got_error *
got_worktree_open(struct got_worktree **worktree, const char *path)
{
	const struct got_error *err = NULL;

	do {
		err = open_worktree(worktree, path);
		if (err && !(err->code == GOT_ERR_ERRNO && errno == ENOENT))
			return err;
		if (*worktree)
			return NULL;
		path = dirname(path);
		if (path == NULL)
			return got_error_from_errno2("dirname", path);
	} while (!((path[0] == '.' || path[0] == '/') && path[1] == '\0'));

	return got_error(GOT_ERR_NOT_WORKTREE);
}

const struct got_error *
got_worktree_close(struct got_worktree *worktree)
{
	const struct got_error *err = NULL;
	free(worktree->repo_path);
	free(worktree->path_prefix);
	free(worktree->base_commit_id);
	free(worktree->head_ref_name);
	if (worktree->lockfd != -1)
		if (close(worktree->lockfd) != 0)
			err = got_error_from_errno2("close",
			    got_worktree_get_root_path(worktree));
	free(worktree->root_path);
	free(worktree->gotconfig_path);
	got_gotconfig_free(worktree->gotconfig);
	free(worktree);
	return err;
}

const char *
got_worktree_get_root_path(struct got_worktree *worktree)
{
	return worktree->root_path;
}

const char *
got_worktree_get_repo_path(struct got_worktree *worktree)
{
	return worktree->repo_path;
}
const char *
got_worktree_get_path_prefix(struct got_worktree *worktree)
{
	return worktree->path_prefix;
}

const struct got_error *
got_worktree_match_path_prefix(int *match, struct got_worktree *worktree,
    const char *path_prefix)
{
	char *absprefix = NULL;

	if (!got_path_is_absolute(path_prefix)) {
		if (asprintf(&absprefix, "/%s", path_prefix) == -1)
			return got_error_from_errno("asprintf");
	}
	*match = (strcmp(absprefix ? absprefix : path_prefix,
	    worktree->path_prefix) == 0);
	free(absprefix);
	return NULL;
}

const char *
got_worktree_get_head_ref_name(struct got_worktree *worktree)
{
	return worktree->head_ref_name;
}

const struct got_error *
got_worktree_set_head_ref(struct got_worktree *worktree,
    struct got_reference *head_ref)
{
	const struct got_error *err = NULL;
	char *path_got = NULL, *head_ref_name = NULL;

	if (asprintf(&path_got, "%s/%s", worktree->root_path,
	    GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error_from_errno("asprintf");
		path_got = NULL;
		goto done;
	}

	head_ref_name = strdup(got_ref_get_name(head_ref));
	if (head_ref_name == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}

	err = write_head_ref(path_got, head_ref);
	if (err)
		goto done;

	free(worktree->head_ref_name);
	worktree->head_ref_name = head_ref_name;
done:
	free(path_got);
	if (err)
		free(head_ref_name);
	return err;
}

struct got_object_id *
got_worktree_get_base_commit_id(struct got_worktree *worktree)
{
	return worktree->base_commit_id;
}

const struct got_error *
got_worktree_set_base_commit_id(struct got_worktree *worktree,
    struct got_repository *repo, struct got_object_id *commit_id)
{
	const struct got_error *err;
	struct got_object *obj = NULL;
	char *id_str = NULL;
	char *path_got = NULL;

	if (asprintf(&path_got, "%s/%s", worktree->root_path,
	    GOT_WORKTREE_GOT_DIR) == -1) {
		err = got_error_from_errno("asprintf");
		path_got = NULL;
		goto done;
	}

	err = got_object_open(&obj, repo, commit_id);
	if (err)
		return err;

	if (obj->type != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	/* Record our base commit. */
	err = got_object_id_str(&id_str, commit_id);
	if (err)
		goto done;
	err = update_meta_file(path_got, GOT_WORKTREE_BASE_COMMIT, id_str);
	if (err)
		goto done;

	free(worktree->base_commit_id);
	worktree->base_commit_id = got_object_id_dup(commit_id);
	if (worktree->base_commit_id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
done:
	if (obj)
		got_object_close(obj);
	free(id_str);
	free(path_got);
	return err;
}

const struct got_gotconfig *
got_worktree_get_gotconfig(struct got_worktree *worktree)
{
	return worktree->gotconfig;
}

static const struct got_error *
lock_worktree(struct got_worktree *worktree, int operation)
{
	if (flock(worktree->lockfd, operation | LOCK_NB) == -1)
		return (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno2("flock",
		    got_worktree_get_root_path(worktree)));
	return NULL;
}

static const struct got_error *
add_dir_on_disk(struct got_worktree *worktree, const char *path)
{
	const struct got_error *err = NULL;
	char *abspath;

	if (asprintf(&abspath, "%s/%s", worktree->root_path, path) == -1)
		return got_error_from_errno("asprintf");

	err = got_path_mkdir(abspath);
	if (err && err->code == GOT_ERR_ERRNO && errno == EEXIST) {
		struct stat sb;
		err = NULL;
		if (lstat(abspath, &sb) == -1) {
			err = got_error_from_errno2("lstat", abspath);
		} else if (!S_ISDIR(sb.st_mode)) {
			/* TODO directory is obstructed; do something */
			err = got_error_path(abspath, GOT_ERR_FILE_OBSTRUCTED);
		}
	}
	free(abspath);
	return err;
}

static const struct got_error *
check_file_contents_equal(int *same, FILE *f1, FILE *f2)
{
	const struct got_error *err = NULL;
	uint8_t fbuf1[8192];
	uint8_t fbuf2[8192];
	size_t flen1 = 0, flen2 = 0;

	*same = 1;

	for (;;) {
		flen1 = fread(fbuf1, 1, sizeof(fbuf1), f1);
		if (flen1 == 0 && ferror(f1)) {
			err = got_error_from_errno("fread");
			break;
		}
		flen2 = fread(fbuf2, 1, sizeof(fbuf2), f2);
		if (flen2 == 0 && ferror(f2)) {
			err = got_error_from_errno("fread");
			break;
		}
		if (flen1 == 0) {
			if (flen2 != 0)
				*same = 0;
			break;
		} else if (flen2 == 0) {
			if (flen1 != 0)
				*same = 0;
			break;
		} else if (flen1 == flen2) {
			if (memcmp(fbuf1, fbuf2, flen2) != 0) {
				*same = 0;
				break;
			}
		} else {
			*same = 0;
			break;
		}
	}

	return err;
}

static const struct got_error *
check_files_equal(int *same, const char *f1_path, const char *f2_path)
{
	const struct got_error *err = NULL;
	struct stat sb;
	size_t size1, size2;
	FILE *f1 = NULL, *f2 = NULL;

	*same = 1;

	if (lstat(f1_path, &sb) != 0) {
		err = got_error_from_errno2("lstat", f1_path);
		goto done;
	}
	size1 = sb.st_size;

	if (lstat(f2_path, &sb) != 0) {
		err = got_error_from_errno2("lstat", f2_path);
		goto done;
	}
	size2 = sb.st_size;

	if (size1 != size2) {
		*same = 0;
		return NULL;
	}

	f1 = fopen(f1_path, "r");
	if (f1 == NULL)
		return got_error_from_errno2("fopen", f1_path);

	f2 = fopen(f2_path, "r");
	if (f2 == NULL) {
		err = got_error_from_errno2("fopen", f2_path);
		goto done;
	}

	err = check_file_contents_equal(same, f1, f2);
done:
	if (f1 && fclose(f1) != 0 && err == NULL)
		err = got_error_from_errno("fclose");
	if (f2 && fclose(f2) != 0 && err == NULL)
		err = got_error_from_errno("fclose");

	return err;
}

/*
 * Perform a 3-way merge where blob_orig acts as the common ancestor,
 * the file at deriv_path acts as the first derived version, and the
 * file on disk acts as the second derived version.
 */
static const struct got_error *
merge_file(int *local_changes_subsumed, struct got_worktree *worktree,
    struct got_blob_object *blob_orig, const char *ondisk_path,
    const char *path, uint16_t st_mode, const char *deriv_path,
    const char *label_orig, const char *label_deriv,
    struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	int merged_fd = -1;
	FILE *f_orig = NULL;
	char *blob_orig_path = NULL;
	char *merged_path = NULL, *base_path = NULL;
	int overlapcnt = 0;
	char *parent;
	char *symlink_path = NULL;
	FILE *symlinkf = NULL;

	*local_changes_subsumed = 0;

	parent = dirname(ondisk_path);
	if (parent == NULL)
		return got_error_from_errno2("dirname", ondisk_path);

	if (asprintf(&base_path, "%s/got-merged", parent) == -1)
		return got_error_from_errno("asprintf");

	err = got_opentemp_named_fd(&merged_path, &merged_fd, base_path);
	if (err)
		goto done;

	free(base_path);
	if (asprintf(&base_path, "%s/got-merge-blob-orig", parent) == -1) {
		err = got_error_from_errno("asprintf");
		base_path = NULL;
		goto done;
	}

	err = got_opentemp_named(&blob_orig_path, &f_orig, base_path);
	if (err)
		goto done;
	if (blob_orig) {
		err = got_object_blob_dump_to_file(NULL, NULL, NULL, f_orig,
		    blob_orig);
		if (err)
			goto done;
	} else {
		/*
		 * If the file has no blob, this is an "add vs add" conflict,
		 * and we simply use an empty ancestor file to make both files
		 * appear in the merged result in their entirety.
		 */
	}

	/* 
	 * In order the run a 3-way merge with a symlink we copy the symlink's
	 * target path into a temporary file and use that file with diff3.
	 */
	if (S_ISLNK(st_mode)) {
		char target_path[PATH_MAX];
		ssize_t target_len;
		size_t n;

		free(base_path);
		if (asprintf(&base_path, "%s/got-symlink-merge",
		    parent) == -1) {
			err = got_error_from_errno("asprintf");
			base_path = NULL;
			goto done;
		}
		err = got_opentemp_named(&symlink_path, &symlinkf, base_path);
		if (err)
			goto done;
		target_len = readlink(ondisk_path, target_path,
		    sizeof(target_path));
		if (target_len == -1) {
			err = got_error_from_errno2("readlink", ondisk_path);
			goto done;
		}
		n = fwrite(target_path, 1, target_len, symlinkf);
		if (n != target_len) {
			err = got_ferror(symlinkf, GOT_ERR_IO);
			goto done;
		}
		if (fflush(symlinkf) == EOF) {
			err = got_error_from_errno2("fflush", symlink_path);
			goto done;
		}
	}

	err = got_merge_diff3(&overlapcnt, merged_fd, deriv_path,
	    blob_orig_path, symlink_path ? symlink_path : ondisk_path,
	    label_deriv, label_orig, NULL);
	if (err)
		goto done;

	err = (*progress_cb)(progress_arg,
	    overlapcnt > 0 ? GOT_STATUS_CONFLICT : GOT_STATUS_MERGE, path);
	if (err)
		goto done;

	if (fsync(merged_fd) != 0) {
		err = got_error_from_errno("fsync");
		goto done;
	}

	/* Check if a clean merge has subsumed all local changes. */
	if (overlapcnt == 0) {
		err = check_files_equal(local_changes_subsumed, deriv_path,
		    merged_path);
		if (err)
			goto done;
	}

	if (fchmod(merged_fd, st_mode) != 0) {
		err = got_error_from_errno2("fchmod", merged_path);
		goto done;
	}

	if (rename(merged_path, ondisk_path) != 0) {
		err = got_error_from_errno3("rename", merged_path,
		    ondisk_path);
		goto done;
	}
done:
	if (err) {
		if (merged_path)
			unlink(merged_path);
	}
	if (symlink_path) {
		if (unlink(symlink_path) == -1 && err == NULL)
			err = got_error_from_errno2("unlink", symlink_path);
	}
	if (symlinkf && fclose(symlinkf) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", symlink_path);
	free(symlink_path);
	if (merged_fd != -1 && close(merged_fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	if (f_orig && fclose(f_orig) != 0 && err == NULL)
		err = got_error_from_errno("fclose");
	free(merged_path);
	free(base_path);
	if (blob_orig_path) {
		unlink(blob_orig_path);
		free(blob_orig_path);
	}
	return err;
}

static const struct got_error *
update_symlink(const char *ondisk_path, const char *target_path,
    size_t target_len)
{
	/* This is not atomic but matches what 'ln -sf' does. */
	if (unlink(ondisk_path) == -1)
		return got_error_from_errno2("unlink", ondisk_path);
	if (symlink(target_path, ondisk_path) == -1)
		return got_error_from_errno3("symlink", target_path,
		    ondisk_path);
	return NULL;
}

/*
 * Overwrite a symlink (or a regular file in case there was a "bad" symlink)
 * in the work tree with a file that contains conflict markers and the
 * conflicting target paths of the original version, a "derived version"
 * of a symlink from an incoming change, and a local version of the symlink.
 *
 * The original versions's target path can be NULL if it is not available,
 * such as if both derived versions added a new symlink at the same path.
 *
 * The incoming derived symlink target is NULL in case the incoming change
 * has deleted this symlink.
 */
static const struct got_error *
install_symlink_conflict(const char *deriv_target,
    struct got_object_id *deriv_base_commit_id, const char *orig_target,
    const char *label_orig, const char *local_target, const char *ondisk_path)
{
	const struct got_error *err;
	char *id_str = NULL, *label_deriv = NULL, *path = NULL;
	FILE *f = NULL;

	err = got_object_id_str(&id_str, deriv_base_commit_id);
	if (err)
		return got_error_from_errno("asprintf");

	if (asprintf(&label_deriv, "%s: commit %s",
	    GOT_MERGE_LABEL_MERGED, id_str) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	err = got_opentemp_named(&path, &f, "got-symlink-conflict");
	if (err)
		goto done;

	if (fprintf(f, "%s %s\n%s\n%s%s%s%s%s\n%s\n%s\n",
	    GOT_DIFF_CONFLICT_MARKER_BEGIN, label_deriv,
	    deriv_target ? deriv_target : "(symlink was deleted)",
	    orig_target ? label_orig : "",
	    orig_target ? "\n" : "",
	    orig_target ? orig_target : "",
	    orig_target ? "\n" : "",
	    GOT_DIFF_CONFLICT_MARKER_SEP,
	    local_target, GOT_DIFF_CONFLICT_MARKER_END) < 0) {
		err = got_error_from_errno2("fprintf", path);
		goto done;
	}

	if (unlink(ondisk_path) == -1) {
		err = got_error_from_errno2("unlink", ondisk_path);
		goto done;
	}
	if (rename(path, ondisk_path) == -1) {
		err = got_error_from_errno3("rename", path, ondisk_path);
		goto done;
	}
	if (chmod(ondisk_path, GOT_DEFAULT_FILE_MODE) == -1) {
		err = got_error_from_errno2("chmod", ondisk_path);
		goto done;
	}
done:
	if (f != NULL && fclose(f) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path);
	free(path);
	free(id_str);
	free(label_deriv);
	return err;
}

/* forward declaration */
static const struct got_error *
merge_blob(int *, struct got_worktree *, struct got_blob_object *,
    const char *, const char *, uint16_t, const char *,
    struct got_blob_object *, struct got_object_id *,
    struct got_repository *, got_worktree_checkout_cb, void *);

/*
 * Merge a symlink into the work tree, where blob_orig acts as the common
 * ancestor, deriv_target is the link target of the first derived version,
 * and the symlink on disk acts as the second derived version.
 * Assume that contents of both blobs represent symlinks.
 */
static const struct got_error *
merge_symlink(struct got_worktree *worktree,
    struct got_blob_object *blob_orig, const char *ondisk_path,
    const char *path, const char *label_orig, const char *deriv_target,
    struct got_object_id *deriv_base_commit_id, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	char *ancestor_target = NULL;
	struct stat sb;
	ssize_t ondisk_len, deriv_len;
	char ondisk_target[PATH_MAX];
	int have_local_change = 0;
	int have_incoming_change = 0;

	if (lstat(ondisk_path, &sb) == -1)
		return got_error_from_errno2("lstat", ondisk_path);

	ondisk_len = readlink(ondisk_path, ondisk_target,
	    sizeof(ondisk_target));
	if (ondisk_len == -1) {
		err = got_error_from_errno2("readlink",
		    ondisk_path);
		goto done;
	}
	ondisk_target[ondisk_len] = '\0';

	if (blob_orig) {
		err = got_object_blob_read_to_str(&ancestor_target, blob_orig);
		if (err)
			goto done;
	}

	if (ancestor_target == NULL ||
	    (ondisk_len != strlen(ancestor_target) ||
	    memcmp(ondisk_target, ancestor_target, ondisk_len) != 0))
		have_local_change = 1;

	deriv_len = strlen(deriv_target);
	if (ancestor_target == NULL ||
	    (deriv_len != strlen(ancestor_target) ||
	    memcmp(deriv_target, ancestor_target, deriv_len) != 0))
		have_incoming_change = 1;

	if (!have_local_change && !have_incoming_change) {
		if (ancestor_target) {
			/* Both sides made the same change. */
			err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE,
			    path);
		} else if (deriv_len == ondisk_len &&
		    memcmp(ondisk_target, deriv_target, deriv_len) == 0) {
			/* Both sides added the same symlink. */
			err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE,
			    path);
		} else {
			/* Both sides added symlinks which don't match. */
			err = install_symlink_conflict(deriv_target,
			    deriv_base_commit_id, ancestor_target,
			    label_orig, ondisk_target, ondisk_path);
			if (err)
				goto done;
			err = (*progress_cb)(progress_arg, GOT_STATUS_CONFLICT,
			    path);
		}
	} else if (!have_local_change && have_incoming_change) {
		/* Apply the incoming change. */
		err = update_symlink(ondisk_path, deriv_target,
		    strlen(deriv_target));
		if (err)
			goto done;
		err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE, path);
	} else if (have_local_change && have_incoming_change) {
		if (deriv_len == ondisk_len &&
		    memcmp(deriv_target, ondisk_target, deriv_len) == 0) {
			/* Both sides made the same change. */
			err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE,
			    path);
		} else {
			err = install_symlink_conflict(deriv_target,
			    deriv_base_commit_id, ancestor_target, label_orig,
			    ondisk_target, ondisk_path);
			if (err)
				goto done;
			err = (*progress_cb)(progress_arg, GOT_STATUS_CONFLICT,
			    path);
		}
	}

done:
	free(ancestor_target);
	return err;
}

/*
 * Perform a 3-way merge where blob_orig acts as the common ancestor,
 * blob_deriv acts as the first derived version, and the file on disk
 * acts as the second derived version.
 */
static const struct got_error *
merge_blob(int *local_changes_subsumed, struct got_worktree *worktree,
    struct got_blob_object *blob_orig, const char *ondisk_path,
    const char *path, uint16_t st_mode, const char *label_orig,
    struct got_blob_object *blob_deriv,
    struct got_object_id *deriv_base_commit_id, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	FILE *f_deriv = NULL;
	char *blob_deriv_path = NULL, *base_path = NULL, *id_str = NULL;
	char *label_deriv = NULL, *parent;

	*local_changes_subsumed = 0;

	parent = dirname(ondisk_path);
	if (parent == NULL)
		return got_error_from_errno2("dirname", ondisk_path);

	free(base_path);
	if (asprintf(&base_path, "%s/got-merge-blob-deriv", parent) == -1) {
		err = got_error_from_errno("asprintf");
		base_path = NULL;
		goto done;
	}

	err = got_opentemp_named(&blob_deriv_path, &f_deriv, base_path);
	if (err)
		goto done;
	err = got_object_blob_dump_to_file(NULL, NULL, NULL, f_deriv,
	    blob_deriv);
	if (err)
		goto done;

	err = got_object_id_str(&id_str, deriv_base_commit_id);
	if (err)
		goto done;
	if (asprintf(&label_deriv, "%s: commit %s",
	    GOT_MERGE_LABEL_MERGED, id_str) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	err = merge_file(local_changes_subsumed, worktree, blob_orig,
	    ondisk_path, path, st_mode, blob_deriv_path, label_orig,
	    label_deriv, repo, progress_cb, progress_arg);
done:
	if (f_deriv && fclose(f_deriv) != 0 && err == NULL)
		err = got_error_from_errno("fclose");
	free(base_path);
	if (blob_deriv_path) {
		unlink(blob_deriv_path);
		free(blob_deriv_path);
	}
	free(id_str);
	free(label_deriv);
	return err;
}

static const struct got_error *
create_fileindex_entry(struct got_fileindex_entry **new_iep,
    struct got_fileindex *fileindex, struct got_object_id *base_commit_id,
    const char *ondisk_path, const char *path, struct got_object_id *blob_id)
{
	const struct got_error *err = NULL;
	struct got_fileindex_entry *new_ie;

	*new_iep = NULL;

	err = got_fileindex_entry_alloc(&new_ie, path);
	if (err)
		return err;

	err = got_fileindex_entry_update(new_ie, ondisk_path,
	    blob_id->sha1, base_commit_id->sha1, 1);
	if (err)
		goto done;

	err = got_fileindex_entry_add(fileindex, new_ie);
done:
	if (err)
		got_fileindex_entry_free(new_ie);
	else
		*new_iep = new_ie;
	return err;
}

static mode_t
get_ondisk_perms(int executable, mode_t st_mode)
{
	mode_t xbits = S_IXUSR;

	if (executable) {
		/* Map read bits to execute bits. */
		if (st_mode & S_IRGRP)
			xbits |= S_IXGRP;
		if (st_mode & S_IROTH)
			xbits |= S_IXOTH;
		return st_mode | xbits;
	}

	return (st_mode & ~(S_IXUSR | S_IXGRP | S_IXOTH));
}

/* forward declaration */
static const struct got_error *
install_blob(struct got_worktree *worktree, const char *ondisk_path,
    const char *path, mode_t te_mode, mode_t st_mode,
    struct got_blob_object *blob, int restoring_missing_file,
    int reverting_versioned_file, int installing_bad_symlink,
    int path_is_unversioned, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg);

/*
 * This function assumes that the provided symlink target points at a
 * safe location in the work tree!
 */
static const struct got_error *
replace_existing_symlink(const char *ondisk_path, const char *target_path,
    size_t target_len)
{
	const struct got_error *err = NULL;
	ssize_t elen;
	char etarget[PATH_MAX];
	int fd;

	/*
	 * "Bad" symlinks (those pointing outside the work tree or into the
	 * .got directory) are installed in the work tree as a regular file
	 * which contains the bad symlink target path.
	 * The new symlink target has already been checked for safety by our
	 * caller. If we can successfully open a regular file then we simply
	 * replace this file with a symlink below.
	 */
	fd = open(ondisk_path, O_RDWR | O_EXCL | O_NOFOLLOW);
	if (fd == -1) {
		if (errno != ELOOP)
			return got_error_from_errno2("open", ondisk_path);

		/* We are updating an existing on-disk symlink. */
		elen = readlink(ondisk_path, etarget, sizeof(etarget));
		if (elen == -1)
			return got_error_from_errno2("readlink", ondisk_path);

		if (elen == target_len &&
		    memcmp(etarget, target_path, target_len) == 0)
			return NULL; /* nothing to do */
	}

	err = update_symlink(ondisk_path, target_path, target_len);
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno2("close", ondisk_path);
	return err;
}

static const struct got_error *
is_bad_symlink_target(int *is_bad_symlink, const char *target_path,
    size_t target_len, const char *ondisk_path, const char *wtroot_path)
{
	const struct got_error *err = NULL;
	char canonpath[PATH_MAX];
	char *path_got = NULL;

	*is_bad_symlink = 0;

	if (target_len >= sizeof(canonpath)) {
		*is_bad_symlink = 1;
		return NULL;
	}

	/*
	 * We do not use realpath(3) to resolve the symlink's target
	 * path because we don't want to resolve symlinks recursively.
	 * Instead we make the path absolute and then canonicalize it.
	 * Relative symlink target lookup should begin at the directory
	 * in which the blob object is being installed.
	 */
	if (!got_path_is_absolute(target_path)) {
		char *abspath;
		char *parent = dirname(ondisk_path);
		if (parent == NULL)
			return got_error_from_errno2("dirname", ondisk_path);
		if (asprintf(&abspath, "%s/%s",  parent, target_path) == -1)
			return got_error_from_errno("asprintf");
		if (strlen(abspath) >= sizeof(canonpath)) {
			err = got_error_path(abspath, GOT_ERR_BAD_PATH);
			free(abspath);
			return err;
		}
		err = got_canonpath(abspath, canonpath, sizeof(canonpath));
		free(abspath);
		if (err)
			return err;
	} else {
		err = got_canonpath(target_path, canonpath, sizeof(canonpath));
		if (err)
			return err;
	}

	/* Only allow symlinks pointing at paths within the work tree. */
	if (!got_path_is_child(canonpath, wtroot_path, strlen(wtroot_path))) {
		*is_bad_symlink = 1;
		return NULL;
	}

	/* Do not allow symlinks pointing into the .got directory. */
	if (asprintf(&path_got, "%s/%s", wtroot_path,
	    GOT_WORKTREE_GOT_DIR) == -1)
		return got_error_from_errno("asprintf");
	if (got_path_is_child(canonpath, path_got, strlen(path_got)))
		*is_bad_symlink = 1;

	free(path_got);
	return NULL;
}

static const struct got_error *
install_symlink(int *is_bad_symlink, struct got_worktree *worktree,
    const char *ondisk_path, const char *path, struct got_blob_object *blob,
    int restoring_missing_file, int reverting_versioned_file,
    int path_is_unversioned, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	char target_path[PATH_MAX];
	size_t len, target_len = 0;
	char *path_got = NULL;
	const uint8_t *buf = got_object_blob_get_read_buf(blob);
	size_t hdrlen = got_object_blob_get_hdrlen(blob);

	*is_bad_symlink = 0;

	/* 
	 * Blob object content specifies the target path of the link.
	 * If a symbolic link cannot be installed we instead create
	 * a regular file which contains the link target path stored
	 * in the blob object.
	 */
	do {
		err = got_object_blob_read_block(&len, blob);
		if (len + target_len >= sizeof(target_path)) {
			/* Path too long; install as a regular file. */
			*is_bad_symlink = 1;
			got_object_blob_rewind(blob);
			return install_blob(worktree, ondisk_path, path,
			    GOT_DEFAULT_FILE_MODE, GOT_DEFAULT_FILE_MODE, blob,
			    restoring_missing_file, reverting_versioned_file,
			    1, path_is_unversioned, repo, progress_cb,
			    progress_arg);
		}
		if (len > 0) {
			/* Skip blob object header first time around. */
			memcpy(target_path + target_len, buf + hdrlen,
			    len - hdrlen);
			target_len += len - hdrlen;
			hdrlen = 0;
		}
	} while (len != 0);
	target_path[target_len] = '\0';

	err = is_bad_symlink_target(is_bad_symlink, target_path, target_len,
	    ondisk_path, worktree->root_path);
	if (err)
		return err;

	if (*is_bad_symlink) {
		/* install as a regular file */
		*is_bad_symlink = 1;
		got_object_blob_rewind(blob);
		err = install_blob(worktree, ondisk_path, path,
		    GOT_DEFAULT_FILE_MODE, GOT_DEFAULT_FILE_MODE, blob,
		    restoring_missing_file, reverting_versioned_file, 1,
		    path_is_unversioned, repo, progress_cb, progress_arg);
		goto done;
	}

	if (symlink(target_path, ondisk_path) == -1) {
		if (errno == EEXIST) {
			if (path_is_unversioned) {
				err = (*progress_cb)(progress_arg,
				    GOT_STATUS_UNVERSIONED, path);
				goto done;
			}
			err = replace_existing_symlink(ondisk_path,
			    target_path, target_len);
			if (err)
				goto done;
			if (progress_cb) {
				err = (*progress_cb)(progress_arg,
				    reverting_versioned_file ?
				    GOT_STATUS_REVERT : GOT_STATUS_UPDATE,
				    path);
			}
			goto done; /* Nothing else to do. */
		}

		if (errno == ENOENT) {
			char *parent = dirname(ondisk_path);
			if (parent == NULL) {
				err = got_error_from_errno2("dirname",
				    ondisk_path);
				goto done;
			}
			err = add_dir_on_disk(worktree, parent);
			if (err)
				goto done;
			/*
			 * Retry, and fall through to error handling
			 * below if this second attempt fails.
			 */
			if (symlink(target_path, ondisk_path) != -1) {
				err = NULL; /* success */
				goto done;
			}
		}

		/* Handle errors from first or second creation attempt. */
		if (errno == ENAMETOOLONG) {
			/* bad target path; install as a regular file */
			*is_bad_symlink = 1;
			got_object_blob_rewind(blob);
			err = install_blob(worktree, ondisk_path, path,
			    GOT_DEFAULT_FILE_MODE, GOT_DEFAULT_FILE_MODE, blob,
			    restoring_missing_file, reverting_versioned_file, 1,
			    path_is_unversioned, repo,
			    progress_cb, progress_arg);
		} else if (errno == ENOTDIR) {
			err = got_error_path(ondisk_path,
			    GOT_ERR_FILE_OBSTRUCTED);
		} else {
			err = got_error_from_errno3("symlink",
			    target_path, ondisk_path);
		}
	} else if (progress_cb)
		err = (*progress_cb)(progress_arg, reverting_versioned_file ?
		    GOT_STATUS_REVERT : GOT_STATUS_ADD, path);
done:
	free(path_got);
	return err;
}

static const struct got_error *
install_blob(struct got_worktree *worktree, const char *ondisk_path,
    const char *path, mode_t te_mode, mode_t st_mode,
    struct got_blob_object *blob, int restoring_missing_file,
    int reverting_versioned_file, int installing_bad_symlink,
    int path_is_unversioned, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	int fd = -1;
	size_t len, hdrlen;
	int update = 0;
	char *tmppath = NULL;

	fd = open(ondisk_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW,
	    GOT_DEFAULT_FILE_MODE);
	if (fd == -1) {
		if (errno == ENOENT) {
			char *parent = dirname(path);
			if (parent == NULL)
				return got_error_from_errno2("dirname", path);
			err = add_dir_on_disk(worktree, parent);
			if (err)
				return err;
			fd = open(ondisk_path,
			    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW,
			    GOT_DEFAULT_FILE_MODE);
			if (fd == -1)
				return got_error_from_errno2("open",
				    ondisk_path);
		} else if (errno == EEXIST) {
			if (path_is_unversioned) {
				err = (*progress_cb)(progress_arg,
				    GOT_STATUS_UNVERSIONED, path);
				goto done;
			}
			if (!S_ISREG(st_mode) && !installing_bad_symlink) {
				/* TODO file is obstructed; do something */
				err = got_error_path(ondisk_path,
				    GOT_ERR_FILE_OBSTRUCTED);
				goto done;
			} else {
				err = got_opentemp_named_fd(&tmppath, &fd,
				    ondisk_path);
				if (err)
					goto done;
				update = 1;
			}
		} else
			return got_error_from_errno2("open", ondisk_path);
	}

	if (progress_cb) {
		if (restoring_missing_file)
			err = (*progress_cb)(progress_arg, GOT_STATUS_MISSING,
			    path);
		else if (reverting_versioned_file)
			err = (*progress_cb)(progress_arg, GOT_STATUS_REVERT,
			    path);
		else
			err = (*progress_cb)(progress_arg,
			    update ? GOT_STATUS_UPDATE : GOT_STATUS_ADD, path);
		if (err)
			goto done;
	}

	hdrlen = got_object_blob_get_hdrlen(blob);
	do {
		const uint8_t *buf = got_object_blob_get_read_buf(blob);
		err = got_object_blob_read_block(&len, blob);
		if (err)
			break;
		if (len > 0) {
			/* Skip blob object header first time around. */
			ssize_t outlen = write(fd, buf + hdrlen, len - hdrlen);
			if (outlen == -1) {
				err = got_error_from_errno("write");
				goto done;
			} else if (outlen != len - hdrlen) {
				err = got_error(GOT_ERR_IO);
				goto done;
			}
			hdrlen = 0;
		}
	} while (len != 0);

	if (fsync(fd) != 0) {
		err = got_error_from_errno("fsync");
		goto done;
	}

	if (update) {
		if (rename(tmppath, ondisk_path) != 0) {
			err = got_error_from_errno3("rename", tmppath,
			    ondisk_path);
			unlink(tmppath);
			goto done;
		}
	}

	if (chmod(ondisk_path,
	    get_ondisk_perms(te_mode & S_IXUSR, st_mode)) == -1) {
		err = got_error_from_errno2("chmod", ondisk_path);
		goto done;
	}

done:
	if (fd != -1 && close(fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	free(tmppath);
	return err;
}

/* Upgrade STATUS_MODIFY to STATUS_CONFLICT if a conflict marker is found. */
static const struct got_error *
get_modified_file_content_status(unsigned char *status, FILE *f)
{
	const struct got_error *err = NULL;
	const char *markers[3] = {
		GOT_DIFF_CONFLICT_MARKER_BEGIN,
		GOT_DIFF_CONFLICT_MARKER_SEP,
		GOT_DIFF_CONFLICT_MARKER_END
	};
	int i = 0;
	char *line;
	size_t len;
	const char delim[3] = {'\0', '\0', '\0'};

	while (*status == GOT_STATUS_MODIFY) {
		line = fparseln(f, &len, NULL, delim, 0);
		if (line == NULL) {
			if (feof(f))
				break;
			err = got_ferror(f, GOT_ERR_IO);
			break;
		}

		if (strncmp(line, markers[i], strlen(markers[i])) == 0) {
			if (strcmp(markers[i], GOT_DIFF_CONFLICT_MARKER_END)
			    == 0)
				*status = GOT_STATUS_CONFLICT;
			else
				i++;
		}
	}

	return err;
}

static int
xbit_differs(struct got_fileindex_entry *ie, uint16_t st_mode)
{
	mode_t ie_mode = got_fileindex_perms_to_st(ie);
	return ((ie_mode & S_IXUSR) != (st_mode & S_IXUSR));
}

static int
stat_info_differs(struct got_fileindex_entry *ie, struct stat *sb)
{
	return !(ie->ctime_sec == sb->st_ctim.tv_sec &&
	    ie->ctime_nsec == sb->st_ctim.tv_nsec &&
	    ie->mtime_sec == sb->st_mtim.tv_sec &&
	    ie->mtime_nsec == sb->st_mtim.tv_nsec &&
	    ie->size == (sb->st_size & 0xffffffff) &&
	    !xbit_differs(ie, sb->st_mode));
}

static unsigned char
get_staged_status(struct got_fileindex_entry *ie)
{
	switch (got_fileindex_entry_stage_get(ie)) {
	case GOT_FILEIDX_STAGE_ADD:
		return GOT_STATUS_ADD;
	case GOT_FILEIDX_STAGE_DELETE:
		return GOT_STATUS_DELETE;
	case GOT_FILEIDX_STAGE_MODIFY:
		return GOT_STATUS_MODIFY;
	default:
		return GOT_STATUS_NO_CHANGE;
	}
}

static const struct got_error *
get_symlink_modification_status(unsigned char *status,
    struct got_fileindex_entry *ie, const char *abspath,
    int dirfd, const char *de_name, struct got_blob_object *blob)
{
	const struct got_error *err = NULL;
	char target_path[PATH_MAX];
	char etarget[PATH_MAX];
	ssize_t elen;
	size_t len, target_len = 0;
	const uint8_t *buf = got_object_blob_get_read_buf(blob);
	size_t hdrlen = got_object_blob_get_hdrlen(blob);

	*status = GOT_STATUS_NO_CHANGE;

	/* Blob object content specifies the target path of the link. */
	do {
		err = got_object_blob_read_block(&len, blob);
		if (err)
			return err;
		if (len + target_len >= sizeof(target_path)) {
			/*
			 * Should not happen. The blob contents were OK
			 * when this symlink was installed.
			 */
			return got_error(GOT_ERR_NO_SPACE);
		}
		if (len > 0) {
			/* Skip blob object header first time around. */
			memcpy(target_path + target_len, buf + hdrlen,
			    len - hdrlen);
			target_len += len - hdrlen;
			hdrlen = 0;
		}
	} while (len != 0);
	target_path[target_len] = '\0';

	if (dirfd != -1) {
		elen = readlinkat(dirfd, de_name, etarget, sizeof(etarget));
		if (elen == -1)
			return got_error_from_errno2("readlinkat", abspath);
	} else {
		elen = readlink(abspath, etarget, sizeof(etarget));
		if (elen == -1)
			return got_error_from_errno2("readlink", abspath);
	}

	if (elen != target_len || memcmp(etarget, target_path, target_len) != 0)
		*status = GOT_STATUS_MODIFY;

	return NULL;
}

static const struct got_error *
get_file_status(unsigned char *status, struct stat *sb,
    struct got_fileindex_entry *ie, const char *abspath,
    int dirfd, const char *de_name, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id id;
	size_t hdrlen;
	int fd = -1;
	FILE *f = NULL;
	uint8_t fbuf[8192];
	struct got_blob_object *blob = NULL;
	size_t flen, blen;
	unsigned char staged_status = get_staged_status(ie);

	*status = GOT_STATUS_NO_CHANGE;

	/*
	 * Whenever the caller provides a directory descriptor and a
	 * directory entry name for the file, use them! This prevents
	 * race conditions if filesystem paths change beneath our feet.
	 */
	if (dirfd != -1) {
		if (fstatat(dirfd, de_name, sb, AT_SYMLINK_NOFOLLOW) == -1) {
			if (errno == ENOENT) {
				if (got_fileindex_entry_has_file_on_disk(ie))
					*status = GOT_STATUS_MISSING;
				else
					*status = GOT_STATUS_DELETE;
				goto done;
			}
			err = got_error_from_errno2("fstatat", abspath);
			goto done;
		}
	} else {
		fd = open(abspath, O_RDONLY | O_NOFOLLOW);
		if (fd == -1 && errno != ENOENT && errno != ELOOP)
			return got_error_from_errno2("open", abspath);
		else if (fd == -1 && errno == ELOOP) {
			if (lstat(abspath, sb) == -1)
				return got_error_from_errno2("lstat", abspath);
		} else if (fd == -1 || fstat(fd, sb) == -1) {
			if (errno == ENOENT) {
				if (got_fileindex_entry_has_file_on_disk(ie))
					*status = GOT_STATUS_MISSING;
				else
					*status = GOT_STATUS_DELETE;
				goto done;
			}
			err = got_error_from_errno2("fstat", abspath);
			goto done;
		}
	}

	if (!S_ISREG(sb->st_mode) && !S_ISLNK(sb->st_mode)) {
		*status = GOT_STATUS_OBSTRUCTED;
		goto done;
	}

	if (!got_fileindex_entry_has_file_on_disk(ie)) {
		*status = GOT_STATUS_DELETE;
		goto done;
	} else if (!got_fileindex_entry_has_blob(ie) &&
	    staged_status != GOT_STATUS_ADD) {
		*status = GOT_STATUS_ADD;
		goto done;
	}

	if (!stat_info_differs(ie, sb))
		goto done;

	if (S_ISLNK(sb->st_mode) &&
	    got_fileindex_entry_filetype_get(ie) != GOT_FILEIDX_MODE_SYMLINK) {
		*status = GOT_STATUS_MODIFY;
		goto done;
	}

	if (staged_status == GOT_STATUS_MODIFY ||
	    staged_status == GOT_STATUS_ADD)
		memcpy(id.sha1, ie->staged_blob_sha1, sizeof(id.sha1));
	else
		memcpy(id.sha1, ie->blob_sha1, sizeof(id.sha1));

	err = got_object_open_as_blob(&blob, repo, &id, sizeof(fbuf));
	if (err)
		goto done;

	if (S_ISLNK(sb->st_mode)) {
		err = get_symlink_modification_status(status, ie,
		    abspath, dirfd, de_name, blob);
		goto done;
	}

	if (dirfd != -1) {
		fd = openat(dirfd, de_name, O_RDONLY | O_NOFOLLOW);
		if (fd == -1) {
			err = got_error_from_errno2("openat", abspath);
			goto done;
		}
	}

	f = fdopen(fd, "r");
	if (f == NULL) {
		err = got_error_from_errno2("fdopen", abspath);
		goto done;
	}
	fd = -1;
	hdrlen = got_object_blob_get_hdrlen(blob);
	for (;;) {
		const uint8_t *bbuf = got_object_blob_get_read_buf(blob);
		err = got_object_blob_read_block(&blen, blob);
		if (err)
			goto done;
		/* Skip length of blob object header first time around. */
		flen = fread(fbuf, 1, sizeof(fbuf) - hdrlen, f);
		if (flen == 0 && ferror(f)) {
			err = got_error_from_errno("fread");
			goto done;
		}
		if (blen == 0) {
			if (flen != 0)
				*status = GOT_STATUS_MODIFY;
			break;
		} else if (flen == 0) {
			if (blen != 0)
				*status = GOT_STATUS_MODIFY;
			break;
		} else if (blen - hdrlen == flen) {
			/* Skip blob object header first time around. */
			if (memcmp(bbuf + hdrlen, fbuf, flen) != 0) {
				*status = GOT_STATUS_MODIFY;
				break;
			}
		} else {
			*status = GOT_STATUS_MODIFY;
			break;
		}
		hdrlen = 0;
	}

	if (*status == GOT_STATUS_MODIFY) {
		rewind(f);
		err = get_modified_file_content_status(status, f);
	} else if (xbit_differs(ie, sb->st_mode))
		*status = GOT_STATUS_MODE_CHANGE;
done:
	if (blob)
		got_object_blob_close(blob);
	if (f != NULL && fclose(f) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", abspath);
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno2("close", abspath);
	return err;
}

/*
 * Update timestamps in the file index if a file is unmodified and
 * we had to run a full content comparison to find out.
 */
static const struct got_error *
sync_timestamps(char *ondisk_path, unsigned char status,
    struct got_fileindex_entry *ie, struct stat *sb)
{
	if (status == GOT_STATUS_NO_CHANGE && stat_info_differs(ie, sb))
		return got_fileindex_entry_update(ie, ondisk_path,
		    ie->blob_sha1, ie->commit_sha1, 1);

	return NULL;
}

static const struct got_error *
update_blob(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_fileindex_entry *ie,
    struct got_tree_entry *te, const char *path,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg)
{
	const struct got_error *err = NULL;
	struct got_blob_object *blob = NULL;
	char *ondisk_path;
	unsigned char status = GOT_STATUS_NO_CHANGE;
	struct stat sb;

	if (asprintf(&ondisk_path, "%s/%s", worktree->root_path, path) == -1)
		return got_error_from_errno("asprintf");

	if (ie) {
		if (get_staged_status(ie) != GOT_STATUS_NO_CHANGE) {
			err = got_error_path(ie->path, GOT_ERR_FILE_STAGED);
			goto done;
		}
		err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL,
		    repo);
		if (err)
			goto done;
		if (status == GOT_STATUS_MISSING || status == GOT_STATUS_DELETE)
			sb.st_mode = got_fileindex_perms_to_st(ie);
	} else {
		sb.st_mode = GOT_DEFAULT_FILE_MODE;
		status = GOT_STATUS_UNVERSIONED;
	}

	if (status == GOT_STATUS_OBSTRUCTED) {
		err = (*progress_cb)(progress_arg, status, path);
		goto done;
	}
	if (status == GOT_STATUS_CONFLICT) {
		err = (*progress_cb)(progress_arg, GOT_STATUS_CANNOT_UPDATE,
		    path);
		goto done;
	}

	if (ie && status != GOT_STATUS_MISSING &&
	    (te->mode & S_IXUSR) == (sb.st_mode & S_IXUSR)) {
		if (got_fileindex_entry_has_commit(ie) &&
		    memcmp(ie->commit_sha1, worktree->base_commit_id->sha1,
		    SHA1_DIGEST_LENGTH) == 0) {
			err = sync_timestamps(ondisk_path, status, ie, &sb);
			if (err)
				goto done;
			err = (*progress_cb)(progress_arg, GOT_STATUS_EXISTS,
			    path);
			goto done;
		}
		if (got_fileindex_entry_has_blob(ie) &&
		    memcmp(ie->blob_sha1, te->id.sha1,
		    SHA1_DIGEST_LENGTH) == 0) {
			err = sync_timestamps(ondisk_path, status, ie, &sb);
			goto done;
		}
	}

	err = got_object_open_as_blob(&blob, repo, &te->id, 8192);
	if (err)
		goto done;

	if (status == GOT_STATUS_MODIFY || status == GOT_STATUS_ADD) {
		int update_timestamps;
		struct got_blob_object *blob2 = NULL;
		char *label_orig = NULL;
		if (got_fileindex_entry_has_blob(ie)) {
			struct got_object_id id2;
			memcpy(id2.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
			err = got_object_open_as_blob(&blob2, repo, &id2, 8192);
			if (err)
				goto done;
		}
		if (got_fileindex_entry_has_commit(ie)) {
			char id_str[SHA1_DIGEST_STRING_LENGTH];
			if (got_sha1_digest_to_str(ie->commit_sha1, id_str,
			    sizeof(id_str)) == NULL) {
				err = got_error_path(id_str,
				    GOT_ERR_BAD_OBJ_ID_STR);
				goto done;
			}
			if (asprintf(&label_orig, "%s: commit %s",
			    GOT_MERGE_LABEL_BASE, id_str) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
		if (S_ISLNK(te->mode) && S_ISLNK(sb.st_mode)) {
			char *link_target;
			err = got_object_blob_read_to_str(&link_target, blob);
			if (err)
				goto done;
			err = merge_symlink(worktree, blob2, ondisk_path, path,
			    label_orig, link_target, worktree->base_commit_id,
			    repo, progress_cb, progress_arg);
			free(link_target);
		} else {
			err = merge_blob(&update_timestamps, worktree, blob2,
			    ondisk_path, path, sb.st_mode, label_orig, blob,
			    worktree->base_commit_id, repo,
			    progress_cb, progress_arg);
		}
		free(label_orig);
		if (blob2)
			got_object_blob_close(blob2);
		if (err)
			goto done;
		/*
		 * Do not update timestamps of files with local changes.
		 * Otherwise, a future status walk would treat them as
		 * unmodified files again.
		 */
		err = got_fileindex_entry_update(ie, ondisk_path,
		    blob->id.sha1, worktree->base_commit_id->sha1,
		    update_timestamps);
	} else if (status == GOT_STATUS_MODE_CHANGE) {
		err = got_fileindex_entry_update(ie, ondisk_path,
		    blob->id.sha1, worktree->base_commit_id->sha1, 0);
	} else if (status == GOT_STATUS_DELETE) {
		err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE, path);
		if (err)
			goto done;
		err = got_fileindex_entry_update(ie, ondisk_path,
		    blob->id.sha1, worktree->base_commit_id->sha1, 0);
		if (err)
			goto done;
	} else {
		int is_bad_symlink = 0;
		if (S_ISLNK(te->mode)) {
			err = install_symlink(&is_bad_symlink, worktree,
			    ondisk_path, path, blob,
			    status == GOT_STATUS_MISSING, 0,
			    status == GOT_STATUS_UNVERSIONED, repo,
			    progress_cb, progress_arg);
		} else {
			err = install_blob(worktree, ondisk_path, path,
			    te->mode, sb.st_mode, blob,
			    status == GOT_STATUS_MISSING, 0, 0,
			    status == GOT_STATUS_UNVERSIONED, repo,
			    progress_cb, progress_arg);
		}
		if (err)
			goto done;

		if (ie) {
			err = got_fileindex_entry_update(ie, ondisk_path,
			    blob->id.sha1, worktree->base_commit_id->sha1, 1);
		} else {
			err = create_fileindex_entry(&ie, fileindex,
			    worktree->base_commit_id, ondisk_path, path,
			    &blob->id);
		}
		if (err)
			goto done;

		if (is_bad_symlink) {
			got_fileindex_entry_filetype_set(ie,
			    GOT_FILEIDX_MODE_BAD_SYMLINK);
		}
	}
	got_object_blob_close(blob);
done:
	free(ondisk_path);
	return err;
}

static const struct got_error *
remove_ondisk_file(const char *root_path, const char *path)
{
	const struct got_error *err = NULL;
	char *ondisk_path = NULL;

	if (asprintf(&ondisk_path, "%s/%s", root_path, path) == -1)
		return got_error_from_errno("asprintf");

	if (unlink(ondisk_path) == -1) {
		if (errno != ENOENT)
			err = got_error_from_errno2("unlink", ondisk_path);
	} else {
		char *parent = dirname(ondisk_path);
		while (parent && strcmp(parent, root_path) != 0) {
			if (rmdir(parent) == -1) {
				if (errno != ENOTEMPTY)
					err = got_error_from_errno2("rmdir",
					    parent);
				break;
			}
			parent = dirname(parent);
		}
	}
	free(ondisk_path);
	return err;
}

static const struct got_error *
delete_blob(struct got_worktree *worktree, struct got_fileindex *fileindex,
    struct got_fileindex_entry *ie, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	unsigned char status;
	struct stat sb;
	char *ondisk_path;

	if (get_staged_status(ie) != GOT_STATUS_NO_CHANGE)
		return got_error_path(ie->path, GOT_ERR_FILE_STAGED);

	if (asprintf(&ondisk_path, "%s/%s", worktree->root_path, ie->path)
	    == -1)
		return got_error_from_errno("asprintf");

	err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL, repo);
	if (err)
		goto done;

	if (S_ISLNK(sb.st_mode) && status != GOT_STATUS_NO_CHANGE) {
		char ondisk_target[PATH_MAX];
		ssize_t ondisk_len = readlink(ondisk_path, ondisk_target,
		    sizeof(ondisk_target));
		if (ondisk_len == -1) {
			err = got_error_from_errno2("readlink", ondisk_path);
			goto done;
		}
		ondisk_target[ondisk_len] = '\0';
		err = install_symlink_conflict(NULL, worktree->base_commit_id,
		    NULL, NULL, /* XXX pass common ancestor info? */
		    ondisk_target, ondisk_path);
		if (err)
			goto done;
		err = (*progress_cb)(progress_arg, GOT_STATUS_CONFLICT,
		    ie->path);
		goto done;
	}

	if (status == GOT_STATUS_MODIFY || status == GOT_STATUS_CONFLICT ||
	    status == GOT_STATUS_ADD) {
		err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE, ie->path);
		if (err)
			goto done;
		/*
		 * Preserve the working file and change the deleted blob's
		 * entry into a schedule-add entry.
		 */
		err = got_fileindex_entry_update(ie, ondisk_path, NULL, NULL,
		    0);
	} else {
		err = (*progress_cb)(progress_arg, GOT_STATUS_DELETE, ie->path);
		if (err)
			goto done;
		if (status == GOT_STATUS_NO_CHANGE) {
			err = remove_ondisk_file(worktree->root_path, ie->path);
			if (err)
				goto done;
		}
		got_fileindex_entry_remove(fileindex, ie);
	}
done:
	free(ondisk_path);
	return err;
}

struct diff_cb_arg {
    struct got_fileindex *fileindex;
    struct got_worktree *worktree;
    struct got_repository *repo;
    got_worktree_checkout_cb progress_cb;
    void *progress_arg;
    got_cancel_cb cancel_cb;
    void *cancel_arg;
};

static const struct got_error *
diff_old_new(void *arg, struct got_fileindex_entry *ie,
    struct got_tree_entry *te, const char *parent_path)
{
	struct diff_cb_arg *a = arg;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	return update_blob(a->worktree, a->fileindex, ie, te,
	    ie->path, a->repo, a->progress_cb, a->progress_arg);
}

static const struct got_error *
diff_old(void *arg, struct got_fileindex_entry *ie, const char *parent_path)
{
	struct diff_cb_arg *a = arg;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	return delete_blob(a->worktree, a->fileindex, ie,
	    a->repo, a->progress_cb, a->progress_arg);
}

static const struct got_error *
diff_new(void *arg, struct got_tree_entry *te, const char *parent_path)
{
	struct diff_cb_arg *a = arg;
	const struct got_error *err;
	char *path;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (got_object_tree_entry_is_submodule(te))
		return NULL;

	if (asprintf(&path, "%s%s%s", parent_path,
	    parent_path[0] ? "/" : "", te->name)
	    == -1)
		return got_error_from_errno("asprintf");

	if (S_ISDIR(te->mode))
		err = add_dir_on_disk(a->worktree, path);
	else
		err = update_blob(a->worktree, a->fileindex, NULL, te, path,
		    a->repo, a->progress_cb, a->progress_arg);

	free(path);
	return err;
}

const struct got_error *
got_worktree_get_uuid(char **uuidstr, struct got_worktree *worktree)
{
	uint32_t uuid_status;

	uuid_to_string(&worktree->uuid, uuidstr, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		*uuidstr = NULL;
		return got_error_uuid(uuid_status, "uuid_to_string");
	}

	return NULL;
}

static const struct got_error *
get_ref_name(char **refname, struct got_worktree *worktree, const char *prefix)
{
	const struct got_error *err = NULL;
	char *uuidstr = NULL;

	*refname = NULL;

	err = got_worktree_get_uuid(&uuidstr, worktree);
	if (err)
		return err;

	if (asprintf(refname, "%s-%s", prefix, uuidstr) == -1) {
		err = got_error_from_errno("asprintf");
		*refname = NULL;
	}
	free(uuidstr);
	return err;
}

const struct got_error *
got_worktree_get_base_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree, GOT_WORKTREE_BASE_REF_PREFIX);
}

static const struct got_error *
get_rebase_tmp_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_REBASE_TMP_REF_PREFIX);
}

static const struct got_error *
get_newbase_symref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree, GOT_WORKTREE_NEWBASE_REF_PREFIX);
}

static const struct got_error *
get_rebase_branch_symref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_REBASE_BRANCH_REF_PREFIX);
}

static const struct got_error *
get_rebase_commit_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_REBASE_COMMIT_REF_PREFIX);
}

static const struct got_error *
get_histedit_tmp_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_HISTEDIT_TMP_REF_PREFIX);
}

static const struct got_error *
get_histedit_branch_symref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_HISTEDIT_BRANCH_REF_PREFIX);
}

static const struct got_error *
get_histedit_base_commit_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_HISTEDIT_BASE_COMMIT_REF_PREFIX);
}

static const struct got_error *
get_histedit_commit_ref_name(char **refname, struct got_worktree *worktree)
{
	return get_ref_name(refname, worktree,
	    GOT_WORKTREE_HISTEDIT_COMMIT_REF_PREFIX);
}

const struct got_error *
got_worktree_get_histedit_script_path(char **path,
    struct got_worktree *worktree)
{
	if (asprintf(path, "%s/%s/%s", worktree->root_path,
	    GOT_WORKTREE_GOT_DIR, GOT_WORKTREE_HISTEDIT_SCRIPT) == -1) {
		*path = NULL;
		return got_error_from_errno("asprintf");
	}
	return NULL;
}

/*
 * Prevent Git's garbage collector from deleting our base commit by
 * setting a reference to our base commit's ID.
 */
static const struct got_error *
ref_base_commit(struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_reference *ref = NULL;
	char *refname;

	err = got_worktree_get_base_ref_name(&refname, worktree);
	if (err)
		return err;

	err = got_ref_alloc(&ref, refname, worktree->base_commit_id);
	if (err)
		goto done;

	err = got_ref_write(ref, repo);
done:
	free(refname);
	if (ref)
		got_ref_close(ref);
	return err;
}

static const struct got_error *
get_fileindex_path(char **fileindex_path, struct got_worktree *worktree)
{
	const struct got_error *err = NULL;

	if (asprintf(fileindex_path, "%s/%s/%s", worktree->root_path,
	    GOT_WORKTREE_GOT_DIR, GOT_WORKTREE_FILE_INDEX) == -1) {
		err = got_error_from_errno("asprintf");
		*fileindex_path = NULL;
	}
	return err;
}


static const struct got_error *
open_fileindex(struct got_fileindex **fileindex, char **fileindex_path,
    struct got_worktree *worktree)
{
	const struct got_error *err = NULL;
	FILE *index = NULL;

	*fileindex_path = NULL;
	*fileindex = got_fileindex_alloc();
	if (*fileindex == NULL)
		return got_error_from_errno("got_fileindex_alloc");

	err = get_fileindex_path(fileindex_path, worktree);
	if (err)
		goto done;

	index = fopen(*fileindex_path, "rb");
	if (index == NULL) {
		if (errno != ENOENT)
			err = got_error_from_errno2("fopen", *fileindex_path);
	} else {
		err = got_fileindex_read(*fileindex, index);
		if (fclose(index) != 0 && err == NULL)
			err = got_error_from_errno("fclose");
	}
done:
	if (err) {
		free(*fileindex_path);
		*fileindex_path = NULL;
		got_fileindex_free(*fileindex);
		*fileindex = NULL;
	}
	return err;
}

struct bump_base_commit_id_arg {
	struct got_object_id *base_commit_id;
	const char *path;
	size_t path_len;
	const char *entry_name;
	got_worktree_checkout_cb progress_cb;
	void *progress_arg;
};

/* Bump base commit ID of all files within an updated part of the work tree. */
static const struct got_error *
bump_base_commit_id(void *arg, struct got_fileindex_entry *ie)
{
	const struct got_error *err;
	struct bump_base_commit_id_arg *a = arg;

	if (a->entry_name) {
		if (strcmp(ie->path, a->path) != 0)
			return NULL;
	} else if (!got_path_is_child(ie->path, a->path, a->path_len))
		return NULL;

	if (memcmp(ie->commit_sha1, a->base_commit_id->sha1,
	    SHA1_DIGEST_LENGTH) == 0)
		return NULL;

	if (a->progress_cb) {
		err = (*a->progress_cb)(a->progress_arg, GOT_STATUS_BUMP_BASE,
		    ie->path);
		if (err)
			return err;
	}
	memcpy(ie->commit_sha1, a->base_commit_id->sha1, SHA1_DIGEST_LENGTH);
	return NULL;
}

static const struct got_error *
sync_fileindex(struct got_fileindex *fileindex, const char *fileindex_path)
{
	const struct got_error *err = NULL;
	char *new_fileindex_path = NULL;
	FILE *new_index = NULL;
	struct timespec timeout;

	err = got_opentemp_named(&new_fileindex_path, &new_index,
	    fileindex_path);
	if (err)
		goto done;

	err = got_fileindex_write(fileindex, new_index);
	if (err)
		goto done;

	if (rename(new_fileindex_path, fileindex_path) != 0) {
		err = got_error_from_errno3("rename", new_fileindex_path,
		    fileindex_path);
		unlink(new_fileindex_path);
	}

	/*
	 * Sleep for a short amount of time to ensure that files modified after
	 * this program exits have a different time stamp from the one which
	 * was recorded in the file index.
	 */
	 timeout.tv_sec = 0;
	 timeout.tv_nsec = 1;
	 nanosleep(&timeout,  NULL);
done:
	if (new_index)
		fclose(new_index);
	free(new_fileindex_path);
	return err;
}

static const struct got_error *
find_tree_entry_for_checkout(int *entry_type, char **tree_relpath,
    struct got_object_id **tree_id, const char *wt_relpath,
    struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id *id = NULL;
	char *in_repo_path = NULL;
	int is_root_wt = got_path_is_root_dir(worktree->path_prefix);

	*entry_type = GOT_OBJ_TYPE_ANY;
	*tree_relpath = NULL;
	*tree_id = NULL;

	if (wt_relpath[0] == '\0') {
		/* Check out all files within the work tree. */
		*entry_type = GOT_OBJ_TYPE_TREE;
		*tree_relpath = strdup("");
		if (*tree_relpath == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		err = got_object_id_by_path(tree_id, repo,
		    worktree->base_commit_id, worktree->path_prefix);
		if (err)
			goto done;
		return NULL;
	}

	/* Check out a subset of files in the work tree. */

	if (asprintf(&in_repo_path, "%s%s%s", worktree->path_prefix,
	    is_root_wt ? "" : "/", wt_relpath) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	err = got_object_id_by_path(&id, repo, worktree->base_commit_id,
	    in_repo_path);
	if (err)
		goto done;

	free(in_repo_path);
	in_repo_path = NULL;

	err = got_object_get_type(entry_type, repo, id);
	if (err)
		goto done;

	if (*entry_type == GOT_OBJ_TYPE_BLOB) {
		/* Check out a single file. */
		if (strchr(wt_relpath, '/')  == NULL) {
			/* Check out a single file in work tree's root dir. */
			in_repo_path = strdup(worktree->path_prefix);
			if (in_repo_path == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
			*tree_relpath = strdup("");
			if (*tree_relpath == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		} else {
			/* Check out a single file in a subdirectory. */
			err = got_path_dirname(tree_relpath, wt_relpath);
			if (err)
				return err;
			if (asprintf(&in_repo_path, "%s%s%s",
			    worktree->path_prefix, is_root_wt ? "" : "/",
			    *tree_relpath) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
		err = got_object_id_by_path(tree_id, repo,
		    worktree->base_commit_id, in_repo_path);
	} else {
		/* Check out all files within a subdirectory. */
		*tree_id = got_object_id_dup(id);
		if (*tree_id == NULL) {
			err = got_error_from_errno("got_object_id_dup");
			goto done;
		}
		*tree_relpath = strdup(wt_relpath);
		if (*tree_relpath == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}
done:
	free(id);
	free(in_repo_path);
	if (err) {
		*entry_type = GOT_OBJ_TYPE_ANY;
		free(*tree_relpath);
		*tree_relpath = NULL;
		free(*tree_id);
		*tree_id = NULL;
	}
	return err;
}

static const struct got_error *
checkout_files(struct got_worktree *worktree, struct got_fileindex *fileindex,
    const char *relpath, struct got_object_id *tree_id, const char *entry_name,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_commit_object *commit = NULL;
	struct got_tree_object *tree = NULL;
	struct got_fileindex_diff_tree_cb diff_cb;
	struct diff_cb_arg arg;

	err = ref_base_commit(worktree, repo);
	if (err) {
		if (!(err->code == GOT_ERR_ERRNO &&
		    (errno == EACCES || errno == EROFS)))
			goto done;
		err = (*progress_cb)(progress_arg,
		    GOT_STATUS_BASE_REF_ERR, worktree->root_path);
		if (err)
			return err;
	}

	err = got_object_open_as_commit(&commit, repo,
	   worktree->base_commit_id);
	if (err)
		goto done;

	err = got_object_open_as_tree(&tree, repo, tree_id);
	if (err)
		goto done;

	if (entry_name &&
	    got_object_tree_find_entry(tree, entry_name) == NULL) {
		err = got_error_path(entry_name, GOT_ERR_NO_TREE_ENTRY);
		goto done;
	}

	diff_cb.diff_old_new = diff_old_new;
	diff_cb.diff_old = diff_old;
	diff_cb.diff_new = diff_new;
	arg.fileindex = fileindex;
	arg.worktree = worktree;
	arg.repo = repo;
	arg.progress_cb = progress_cb;
	arg.progress_arg = progress_arg;
	arg.cancel_cb = cancel_cb;
	arg.cancel_arg = cancel_arg;
	err = got_fileindex_diff_tree(fileindex, tree, relpath,
	    entry_name, repo, &diff_cb, &arg);
done:
	if (tree)
		got_object_tree_close(tree);
	if (commit)
		got_object_commit_close(commit);
	return err;
}

const struct got_error *
got_worktree_checkout_files(struct got_worktree *worktree,
    struct got_pathlist_head *paths, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_commit_object *commit = NULL;
	struct got_tree_object *tree = NULL;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	struct got_pathlist_entry *pe;
	struct tree_path_data {
		SIMPLEQ_ENTRY(tree_path_data) entry;
		struct got_object_id *tree_id;
		int entry_type;
		char *relpath;
		char *entry_name;
	} *tpd = NULL;
	SIMPLEQ_HEAD(tree_paths, tree_path_data) tree_paths;

	SIMPLEQ_INIT(&tree_paths);

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	/* Map all specified paths to in-repository trees. */
	TAILQ_FOREACH(pe, paths, entry) {
		tpd = malloc(sizeof(*tpd));
		if (tpd == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}

		err = find_tree_entry_for_checkout(&tpd->entry_type,
		    &tpd->relpath, &tpd->tree_id, pe->path, worktree, repo);
		if (err) {
			free(tpd);
			goto done;
		}

		if (tpd->entry_type == GOT_OBJ_TYPE_BLOB) {
			err = got_path_basename(&tpd->entry_name, pe->path);
			if (err) {
				free(tpd->relpath);
				free(tpd->tree_id);
				free(tpd);
				goto done;
			}
		} else
			tpd->entry_name = NULL;

		SIMPLEQ_INSERT_TAIL(&tree_paths, tpd, entry);
	}

	/*
	 * Read the file index.
	 * Checking out files is supposed to be an idempotent operation.
	 * If the on-disk file index is incomplete we will try to complete it.
	 */
	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	tpd = SIMPLEQ_FIRST(&tree_paths);
	TAILQ_FOREACH(pe, paths, entry) {
		struct bump_base_commit_id_arg bbc_arg;

		err = checkout_files(worktree, fileindex, tpd->relpath,
		    tpd->tree_id, tpd->entry_name, repo,
		    progress_cb, progress_arg, cancel_cb, cancel_arg);
		if (err)
			break;

		bbc_arg.base_commit_id = worktree->base_commit_id;
		bbc_arg.entry_name = tpd->entry_name;
		bbc_arg.path = pe->path;
		bbc_arg.path_len = pe->path_len;
		bbc_arg.progress_cb = progress_cb;
		bbc_arg.progress_arg = progress_arg;
		err = got_fileindex_for_each_entry_safe(fileindex,
		    bump_base_commit_id, &bbc_arg);
		if (err)
			break;

		tpd = SIMPLEQ_NEXT(tpd, entry);
	}
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	if (tree)
		got_object_tree_close(tree);
	if (commit)
		got_object_commit_close(commit);
	if (fileindex)
		got_fileindex_free(fileindex);
	while (!SIMPLEQ_EMPTY(&tree_paths)) {
		tpd = SIMPLEQ_FIRST(&tree_paths);
		SIMPLEQ_REMOVE_HEAD(&tree_paths, entry);
		free(tpd->relpath);
		free(tpd->tree_id);
		free(tpd);
	}
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

struct merge_file_cb_arg {
    struct got_worktree *worktree;
    struct got_fileindex *fileindex;
    got_worktree_checkout_cb progress_cb;
    void *progress_arg;
    got_cancel_cb cancel_cb;
    void *cancel_arg;
    const char *label_orig;
    struct got_object_id *commit_id2;
};

static const struct got_error *
merge_file_cb(void *arg, struct got_blob_object *blob1,
    struct got_blob_object *blob2, struct got_object_id *id1,
    struct got_object_id *id2, const char *path1, const char *path2,
    mode_t mode1, mode_t mode2, struct got_repository *repo)
{
	static const struct got_error *err = NULL;
	struct merge_file_cb_arg *a = arg;
	struct got_fileindex_entry *ie;
	char *ondisk_path = NULL;
	struct stat sb;
	unsigned char status;
	int local_changes_subsumed;

	if (blob1 && blob2) {
		ie = got_fileindex_entry_get(a->fileindex, path2,
		    strlen(path2));
		if (ie == NULL)
			return (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_MISSING, path2);

		if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
		    path2) == -1)
			return got_error_from_errno("asprintf");

		err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL,
		    repo);
		if (err)
			goto done;

		if (status == GOT_STATUS_DELETE) {
			err = (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_MERGE, path2);
			goto done;
		}
		if (status != GOT_STATUS_NO_CHANGE &&
		    status != GOT_STATUS_MODIFY &&
		    status != GOT_STATUS_CONFLICT &&
		    status != GOT_STATUS_ADD) {
			err = (*a->progress_cb)(a->progress_arg, status, path2);
			goto done;
		}

		if (S_ISLNK(mode1) && S_ISLNK(mode2)) {
			char *link_target2;
			err = got_object_blob_read_to_str(&link_target2, blob2);
			if (err)
				goto done;
			err = merge_symlink(a->worktree, blob1, ondisk_path,
			    path2, a->label_orig, link_target2, a->commit_id2,
			    repo, a->progress_cb, a->progress_arg);
			free(link_target2);
		} else {
			err = merge_blob(&local_changes_subsumed, a->worktree,
			    blob1, ondisk_path, path2, sb.st_mode,
			    a->label_orig, blob2, a->commit_id2, repo,
			    a->progress_cb, a->progress_arg);
		}
	} else if (blob1) {
		ie = got_fileindex_entry_get(a->fileindex, path1,
		    strlen(path1));
		if (ie == NULL)
			return (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_MISSING, path1);

		if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
		    path1) == -1)
			return got_error_from_errno("asprintf");

		err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL,
		    repo);
		if (err)
			goto done;

		switch (status) {
		case GOT_STATUS_NO_CHANGE:
			err = (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_DELETE, path1);
			if (err)
				goto done;
			err = remove_ondisk_file(a->worktree->root_path, path1);
			if (err)
				goto done;
			if (ie)
				got_fileindex_entry_mark_deleted_from_disk(ie);
			break;
		case GOT_STATUS_DELETE:
		case GOT_STATUS_MISSING:
			err = (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_DELETE, path1);
			if (err)
				goto done;
			if (ie)
				got_fileindex_entry_mark_deleted_from_disk(ie);
			break;
		case GOT_STATUS_ADD: {
			struct got_object_id *id;
			FILE *blob1_f;
			/*
			 * Delete the added file only if its content already
			 * exists in the repository.
			 */
			err = got_object_blob_file_create(&id, &blob1_f, path1);
			if (err)
				goto done;
			if (got_object_id_cmp(id, id1) == 0) {
				err = (*a->progress_cb)(a->progress_arg,
				    GOT_STATUS_DELETE, path1);
				if (err)
					goto done;
				err = remove_ondisk_file(a->worktree->root_path,
				    path1);
				if (err)
					goto done;
				if (ie)
					got_fileindex_entry_remove(a->fileindex,
					    ie);
			} else {
				err = (*a->progress_cb)(a->progress_arg,
				    GOT_STATUS_CANNOT_DELETE, path1);
			}
			if (fclose(blob1_f) == EOF && err == NULL)
				err = got_error_from_errno("fclose");
			free(id);
			if (err)
				goto done;
			break;
		}
		case GOT_STATUS_MODIFY:
		case GOT_STATUS_CONFLICT:
			err = (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_CANNOT_DELETE, path1);
			if (err)
				goto done;
			break;
		case GOT_STATUS_OBSTRUCTED:
			err = (*a->progress_cb)(a->progress_arg, status, path1);
			if (err)
				goto done;
			break;
		default:
			break;
		}
	} else if (blob2) {
		if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
		    path2) == -1)
			return got_error_from_errno("asprintf");
		ie = got_fileindex_entry_get(a->fileindex, path2,
		    strlen(path2));
		if (ie) {
			err = get_file_status(&status, &sb, ie, ondisk_path,
			    -1, NULL, repo);
			if (err)
				goto done;
			if (status != GOT_STATUS_NO_CHANGE &&
			    status != GOT_STATUS_MODIFY &&
			    status != GOT_STATUS_CONFLICT &&
			    status != GOT_STATUS_ADD) {
				err = (*a->progress_cb)(a->progress_arg,
				    status, path2);
				goto done;
			}
			if (S_ISLNK(mode2) && S_ISLNK(sb.st_mode)) {
				char *link_target2;
				err = got_object_blob_read_to_str(&link_target2,
				    blob2);
				if (err)
					goto done;
				err = merge_symlink(a->worktree, NULL,
				    ondisk_path, path2, a->label_orig,
				    link_target2, a->commit_id2, repo,
				    a->progress_cb, a->progress_arg);
				free(link_target2);
			} else if (S_ISREG(sb.st_mode)) {
				err = merge_blob(&local_changes_subsumed,
				    a->worktree, NULL, ondisk_path, path2,
				    sb.st_mode, a->label_orig, blob2,
				    a->commit_id2, repo, a->progress_cb,
				    a->progress_arg);
			} else {
				err = got_error_path(ondisk_path,
				    GOT_ERR_FILE_OBSTRUCTED);
			}
			if (err)
				goto done;
			if (status == GOT_STATUS_DELETE) {
				err = got_fileindex_entry_update(ie,
				    ondisk_path, blob2->id.sha1,
				    a->worktree->base_commit_id->sha1, 0);
				if (err)
					goto done;
			}
		} else {
			int is_bad_symlink = 0;
			sb.st_mode = GOT_DEFAULT_FILE_MODE;
			if (S_ISLNK(mode2)) {
				err = install_symlink(&is_bad_symlink,
				    a->worktree, ondisk_path, path2, blob2, 0,
				    0, 1, repo, a->progress_cb, a->progress_arg);
			} else {
				err = install_blob(a->worktree, ondisk_path, path2,
				    mode2, sb.st_mode, blob2, 0, 0, 0, 1, repo,
				    a->progress_cb, a->progress_arg);
			}
			if (err)
				goto done;
			err = got_fileindex_entry_alloc(&ie, path2);
			if (err)
				goto done;
			err = got_fileindex_entry_update(ie, ondisk_path,
			    NULL, NULL, 1);
			if (err) {
				got_fileindex_entry_free(ie);
				goto done;
			}
			err = got_fileindex_entry_add(a->fileindex, ie);
			if (err) {
				got_fileindex_entry_free(ie);
				goto done;
			}
			if (is_bad_symlink) {
				got_fileindex_entry_filetype_set(ie,
				    GOT_FILEIDX_MODE_BAD_SYMLINK);
			}
		}
	}
done:
	free(ondisk_path);
	return err;
}

struct check_merge_ok_arg {
	struct got_worktree *worktree;
	struct got_repository *repo;
};

static const struct got_error *
check_merge_ok(void *arg, struct got_fileindex_entry *ie)
{
	const struct got_error *err = NULL;
	struct check_merge_ok_arg *a = arg;
	unsigned char status;
	struct stat sb;
	char *ondisk_path;

	/* Reject merges into a work tree with mixed base commits. */
	if (memcmp(ie->commit_sha1, a->worktree->base_commit_id->sha1,
	    SHA1_DIGEST_LENGTH))
		return got_error(GOT_ERR_MIXED_COMMITS);

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path, ie->path)
	    == -1)
		return got_error_from_errno("asprintf");

	/* Reject merges into a work tree with conflicted files. */
	err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL, a->repo);
	if (err)
		return err;
	if (status == GOT_STATUS_CONFLICT)
		return got_error(GOT_ERR_CONFLICTS);

	return NULL;
}

static const struct got_error *
merge_files(struct got_worktree *worktree, struct got_fileindex *fileindex,
    const char *fileindex_path, struct got_object_id *commit_id1,
    struct got_object_id *commit_id2, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL, *sync_err;
	struct got_object_id *tree_id1 = NULL, *tree_id2 = NULL;
	struct got_tree_object *tree1 = NULL, *tree2 = NULL;
	struct merge_file_cb_arg arg;
	char *label_orig = NULL;

	if (commit_id1) {
		err = got_object_id_by_path(&tree_id1, repo, commit_id1,
		    worktree->path_prefix);
		if (err && err->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
	}
	if (tree_id1) {
		char *id_str;

		err = got_object_open_as_tree(&tree1, repo, tree_id1);
		if (err)
			goto done;

		err = got_object_id_str(&id_str, commit_id1);
		if (err)
			goto done;

		if (asprintf(&label_orig, "%s: commit %s",
		    GOT_MERGE_LABEL_BASE, id_str) == -1) {
			err = got_error_from_errno("asprintf");
			free(id_str);
			goto done;
		}
		free(id_str);
	}

	err = got_object_id_by_path(&tree_id2, repo, commit_id2,
	    worktree->path_prefix);
	if (err)
		goto done;

	err = got_object_open_as_tree(&tree2, repo, tree_id2);
	if (err)
		goto done;

	arg.worktree = worktree;
	arg.fileindex = fileindex;
	arg.progress_cb = progress_cb;
	arg.progress_arg = progress_arg;
	arg.cancel_cb = cancel_cb;
	arg.cancel_arg = cancel_arg;
	arg.label_orig = label_orig;
	arg.commit_id2 = commit_id2;
	err = got_diff_tree(tree1, tree2, "", "", repo, merge_file_cb, &arg, 1);
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	if (tree1)
		got_object_tree_close(tree1);
	if (tree2)
		got_object_tree_close(tree2);
	free(label_orig);
	return err;
}

const struct got_error *
got_worktree_merge_files(struct got_worktree *worktree,
    struct got_object_id *commit_id1, struct got_object_id *commit_id2,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err, *unlockerr;
	char *fileindex_path = NULL;
	struct got_fileindex *fileindex = NULL;
	struct check_merge_ok_arg mok_arg;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	mok_arg.worktree = worktree;
	mok_arg.repo = repo;
	err = got_fileindex_for_each_entry_safe(fileindex, check_merge_ok,
	    &mok_arg);
	if (err)
		goto done;

	err = merge_files(worktree, fileindex, fileindex_path, commit_id1,
	    commit_id2, repo, progress_cb, progress_arg, cancel_cb, cancel_arg);
done:
	if (fileindex)
		got_fileindex_free(fileindex);
	free(fileindex_path);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

struct diff_dir_cb_arg {
    struct got_fileindex *fileindex;
    struct got_worktree *worktree;
    const char *status_path;
    size_t status_path_len;
    struct got_repository *repo;
    got_worktree_status_cb status_cb;
    void *status_arg;
    got_cancel_cb cancel_cb;
    void *cancel_arg;
    /* A pathlist containing per-directory pathlists of ignore patterns. */
    struct got_pathlist_head ignores;
    int report_unchanged;
    int no_ignores;
};

static const struct got_error *
report_file_status(struct got_fileindex_entry *ie, const char *abspath,
    int dirfd, const char *de_name,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo, int report_unchanged)
{
	const struct got_error *err = NULL;
	unsigned char status = GOT_STATUS_NO_CHANGE;
	unsigned char staged_status = get_staged_status(ie);
	struct stat sb;
	struct got_object_id blob_id, commit_id, staged_blob_id;
	struct got_object_id *blob_idp = NULL, *commit_idp = NULL;
	struct got_object_id *staged_blob_idp = NULL;

	err = get_file_status(&status, &sb, ie, abspath, dirfd, de_name, repo);
	if (err)
		return err;

	if (status == GOT_STATUS_NO_CHANGE &&
	    staged_status == GOT_STATUS_NO_CHANGE && !report_unchanged)
		return NULL;

	if (got_fileindex_entry_has_blob(ie)) {
		memcpy(blob_id.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
		blob_idp = &blob_id;
	}
	if (got_fileindex_entry_has_commit(ie)) {
		memcpy(commit_id.sha1, ie->commit_sha1, SHA1_DIGEST_LENGTH);
		commit_idp = &commit_id;
	}
	if (staged_status == GOT_STATUS_ADD ||
	    staged_status == GOT_STATUS_MODIFY) {
		memcpy(staged_blob_id.sha1, ie->staged_blob_sha1,
		    SHA1_DIGEST_LENGTH);
		staged_blob_idp = &staged_blob_id;
	}

	return (*status_cb)(status_arg, status, staged_status,
	    ie->path, blob_idp, staged_blob_idp, commit_idp, dirfd, de_name);
}

static const struct got_error *
status_old_new(void *arg, struct got_fileindex_entry *ie,
    struct dirent *de, const char *parent_path, int dirfd)
{
	const struct got_error *err = NULL;
	struct diff_dir_cb_arg *a = arg;
	char *abspath;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (got_path_cmp(parent_path, a->status_path,
	    strlen(parent_path), a->status_path_len) != 0 &&
	    !got_path_is_child(parent_path, a->status_path, a->status_path_len))
		return NULL;

	if (parent_path[0]) {
		if (asprintf(&abspath, "%s/%s/%s", a->worktree->root_path,
		    parent_path, de->d_name) == -1)
			return got_error_from_errno("asprintf");
	} else {
		if (asprintf(&abspath, "%s/%s", a->worktree->root_path,
		    de->d_name) == -1)
			return got_error_from_errno("asprintf");
	}

	err = report_file_status(ie, abspath, dirfd, de->d_name,
	    a->status_cb, a->status_arg, a->repo, a->report_unchanged);
	free(abspath);
	return err;
}

static const struct got_error *
status_old(void *arg, struct got_fileindex_entry *ie, const char *parent_path)
{
	struct diff_dir_cb_arg *a = arg;
	struct got_object_id blob_id, commit_id;
	unsigned char status;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (!got_path_is_child(ie->path, a->status_path, a->status_path_len))
		return NULL;

	memcpy(blob_id.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
	memcpy(commit_id.sha1, ie->commit_sha1, SHA1_DIGEST_LENGTH);
	if (got_fileindex_entry_has_file_on_disk(ie))
		status = GOT_STATUS_MISSING;
	else
		status = GOT_STATUS_DELETE;
	return (*a->status_cb)(a->status_arg, status, get_staged_status(ie),
	    ie->path, &blob_id, NULL, &commit_id, -1, NULL);
}

void
free_ignorelist(struct got_pathlist_head *ignorelist)
{
	struct got_pathlist_entry *pe;

	TAILQ_FOREACH(pe, ignorelist, entry)
		free((char *)pe->path);
	got_pathlist_free(ignorelist);
}

void
free_ignores(struct got_pathlist_head *ignores)
{
	struct got_pathlist_entry *pe;

	TAILQ_FOREACH(pe, ignores, entry) {
		struct got_pathlist_head *ignorelist = pe->data;
		free_ignorelist(ignorelist);
		free((char *)pe->path);
	}
	got_pathlist_free(ignores);
}

static const struct got_error *
read_ignores(struct got_pathlist_head *ignores, const char *path, FILE *f)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe = NULL;
	struct got_pathlist_head *ignorelist;
	char *line = NULL, *pattern, *dirpath = NULL;
	size_t linesize = 0;
	ssize_t linelen;

	ignorelist = calloc(1, sizeof(*ignorelist));
	if (ignorelist == NULL)
		return got_error_from_errno("calloc");
	TAILQ_INIT(ignorelist);

	while ((linelen = getline(&line, &linesize, f)) != -1) {
		if (linelen > 0 && line[linelen - 1] == '\n')
			line[linelen - 1] = '\0';

		/* Git's ignores may contain comments. */
		if (line[0] == '#')
			continue;

		/* Git's negated patterns are not (yet?) supported. */
		if (line[0] == '!')
			continue;

		if (asprintf(&pattern, "%s%s%s", path, path[0] ? "/" : "",
		    line) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		err = got_pathlist_insert(NULL, ignorelist, pattern, NULL);
		if (err)
			goto done;
	}
	if (ferror(f)) {
		err = got_error_from_errno("getline");
		goto done;
	}

	dirpath = strdup(path);
	if (dirpath == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}
	err = got_pathlist_insert(&pe, ignores, dirpath, ignorelist);
done:
	free(line);
	if (err || pe == NULL) {
		free(dirpath);
		free_ignorelist(ignorelist);
	}
	return err;
}

int
match_ignores(struct got_pathlist_head *ignores, const char *path)
{
	struct got_pathlist_entry *pe;

	/* Handle patterns which match in all directories. */
	TAILQ_FOREACH(pe, ignores, entry) {
		struct got_pathlist_head *ignorelist = pe->data;
		struct got_pathlist_entry *pi;

		TAILQ_FOREACH(pi, ignorelist, entry) {
			const char *p, *pattern = pi->path;

			if (strncmp(pattern, "**/", 3) != 0)
				continue;
			pattern += 3;
			p = path;
			while (*p) {
				if (fnmatch(pattern, p,
				    FNM_PATHNAME | FNM_LEADING_DIR)) {
					/* Retry in next directory. */
					while (*p && *p != '/')
						p++;
					while (*p == '/')
						p++;
					continue;
				}
				return 1;
			}
		}
	}

	/*
	 * The ignores pathlist contains ignore lists from children before
	 * parents, so we can find the most specific ignorelist by walking
	 * ignores backwards.
	 */
	pe = TAILQ_LAST(ignores, got_pathlist_head);
	while (pe) {
		if (got_path_is_child(path, pe->path, pe->path_len)) {
			struct got_pathlist_head *ignorelist = pe->data;
			struct got_pathlist_entry *pi;
			TAILQ_FOREACH(pi, ignorelist, entry) {
				const char *pattern = pi->path;
				int flags = FNM_LEADING_DIR;
				if (strstr(pattern, "/**/") == NULL)
					flags |= FNM_PATHNAME;
				if (fnmatch(pattern, path, flags))
					continue;
				return 1;
			}
		}
		pe = TAILQ_PREV(pe, got_pathlist_head, entry);
	}

	return 0;
}

static const struct got_error *
add_ignores(struct got_pathlist_head *ignores, const char *root_path,
    const char *path, int dirfd, const char *ignores_filename)
{
	const struct got_error *err = NULL;
	char *ignorespath;
	int fd = -1;
	FILE *ignoresfile = NULL;

	if (asprintf(&ignorespath, "%s/%s%s%s", root_path, path,
	    path[0] ? "/" : "", ignores_filename) == -1)
		return got_error_from_errno("asprintf");

	if (dirfd != -1) {
		fd = openat(dirfd, ignores_filename, O_RDONLY | O_NOFOLLOW);
		if (fd == -1) {
			if (errno != ENOENT && errno != EACCES)
				err = got_error_from_errno2("openat",
				    ignorespath);
		} else {
			ignoresfile = fdopen(fd, "r");
			if (ignoresfile == NULL) 
				err = got_error_from_errno2("fdopen",
				    ignorespath);
			else {
				fd = -1;
				err = read_ignores(ignores, path, ignoresfile);
			}
		}
	} else {
		ignoresfile = fopen(ignorespath, "r");
		if (ignoresfile == NULL) {
			if (errno != ENOENT && errno != EACCES)
				err = got_error_from_errno2("fopen",
				    ignorespath);
		} else
			err = read_ignores(ignores, path, ignoresfile);
	}

	if (ignoresfile && fclose(ignoresfile) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path);
	if (fd != -1 && close(fd) == -1 && err == NULL)
		err = got_error_from_errno2("close", path);
	free(ignorespath);
	return err;
}

static const struct got_error *
status_new(void *arg, struct dirent *de, const char *parent_path, int dirfd)
{
	const struct got_error *err = NULL;
	struct diff_dir_cb_arg *a = arg;
	char *path = NULL;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (parent_path[0]) {
		if (asprintf(&path, "%s/%s", parent_path, de->d_name) == -1)
			return got_error_from_errno("asprintf");
	} else {
		path = de->d_name;
	}

	if (de->d_type != DT_DIR &&
	    got_path_is_child(path, a->status_path, a->status_path_len)
	    && !match_ignores(&a->ignores, path))
		err = (*a->status_cb)(a->status_arg, GOT_STATUS_UNVERSIONED,
		    GOT_STATUS_NO_CHANGE, path, NULL, NULL, NULL, -1, NULL);
	if (parent_path[0])
		free(path);
	return err;
}

static const struct got_error *
status_traverse(void *arg, const char *path, int dirfd)
{
	const struct got_error *err = NULL;
	struct diff_dir_cb_arg *a = arg;

	if (a->no_ignores)
		return NULL;

	err = add_ignores(&a->ignores, a->worktree->root_path,
	    path, dirfd, ".cvsignore");
	if (err)
		return err;

	err = add_ignores(&a->ignores, a->worktree->root_path, path,
	    dirfd, ".gitignore");

	return err;
}

static const struct got_error *
report_single_file_status(const char *path, const char *ondisk_path,
struct got_fileindex *fileindex, got_worktree_status_cb status_cb,
void *status_arg, struct got_repository *repo, int report_unchanged)
{
	struct got_fileindex_entry *ie;
	struct stat sb;

	ie = got_fileindex_entry_get(fileindex, path, strlen(path));
	if (ie)
		return report_file_status(ie, ondisk_path, -1, NULL,
		    status_cb, status_arg, repo, report_unchanged);

	if (lstat(ondisk_path, &sb) == -1) {
		if (errno != ENOENT)
			return got_error_from_errno2("lstat", ondisk_path);
		return (*status_cb)(status_arg, GOT_STATUS_NONEXISTENT,
		    GOT_STATUS_NO_CHANGE, path, NULL, NULL, NULL, -1, NULL);
		return NULL;
	}

	if (S_ISREG(sb.st_mode) || S_ISLNK(sb.st_mode))
		return (*status_cb)(status_arg, GOT_STATUS_UNVERSIONED,
		    GOT_STATUS_NO_CHANGE, path, NULL, NULL, NULL, -1, NULL);

	return NULL;
}

static const struct got_error *
add_ignores_from_parent_paths(struct got_pathlist_head *ignores,
    const char *root_path, const char *path)
{
	const struct got_error *err;
	char *parent_path, *next_parent_path = NULL;

	err = add_ignores(ignores, root_path, "", -1,
	    ".cvsignore");
	if (err)
		return err;

	err = add_ignores(ignores, root_path, "", -1,
	    ".gitignore");
	if (err)
		return err;

	err = got_path_dirname(&parent_path, path);
	if (err) {
		if (err->code == GOT_ERR_BAD_PATH)
			return NULL; /* cannot traverse parent */
		return err;
	}
	for (;;) {
		err = add_ignores(ignores, root_path, parent_path, -1,
		    ".cvsignore");
		if (err)
			break;
		err = add_ignores(ignores, root_path, parent_path, -1,
		    ".gitignore");
		if (err)
			break;
		err = got_path_dirname(&next_parent_path, parent_path);
		if (err) {
			if (err->code == GOT_ERR_BAD_PATH)
				err = NULL; /* traversed everything */
			break;
		}
		free(parent_path);
		parent_path = next_parent_path;
		next_parent_path = NULL;
	}

	free(parent_path);
	free(next_parent_path);
	return err;
}

static const struct got_error *
worktree_status(struct got_worktree *worktree, const char *path,
    struct got_fileindex *fileindex, struct got_repository *repo,
    got_worktree_status_cb status_cb, void *status_arg,
    got_cancel_cb cancel_cb, void *cancel_arg, int no_ignores,
    int report_unchanged)
{
	const struct got_error *err = NULL;
	int fd = -1;
	struct got_fileindex_diff_dir_cb fdiff_cb;
	struct diff_dir_cb_arg arg;
	char *ondisk_path = NULL;

	TAILQ_INIT(&arg.ignores);

	if (asprintf(&ondisk_path, "%s%s%s",
	    worktree->root_path, path[0] ? "/" : "", path) == -1)
		return got_error_from_errno("asprintf");

	fd = open(ondisk_path, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
	if (fd == -1) {
		if (errno != ENOTDIR && errno != ENOENT && errno != EACCES &&
		    errno != ELOOP)
			err = got_error_from_errno2("open", ondisk_path);
		else
			err = report_single_file_status(path, ondisk_path,
			    fileindex, status_cb, status_arg, repo,
			    report_unchanged);
	} else {
		fdiff_cb.diff_old_new = status_old_new;
		fdiff_cb.diff_old = status_old;
		fdiff_cb.diff_new = status_new;
		fdiff_cb.diff_traverse = status_traverse;
		arg.fileindex = fileindex;
		arg.worktree = worktree;
		arg.status_path = path;
		arg.status_path_len = strlen(path);
		arg.repo = repo;
		arg.status_cb = status_cb;
		arg.status_arg = status_arg;
		arg.cancel_cb = cancel_cb;
		arg.cancel_arg = cancel_arg;
		arg.report_unchanged = report_unchanged;
		arg.no_ignores = no_ignores;
		if (!no_ignores) {
			err = add_ignores_from_parent_paths(&arg.ignores,
			    worktree->root_path, path);
			if (err)
				goto done;
		}
		err = got_fileindex_diff_dir(fileindex, fd,
		    worktree->root_path, path, repo, &fdiff_cb, &arg);
	}
done:
	free_ignores(&arg.ignores);
	if (fd != -1 && close(fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	free(ondisk_path);
	return err;
}

const struct got_error *
got_worktree_status(struct got_worktree *worktree,
    struct got_pathlist_head *paths, struct got_repository *repo,
    got_worktree_status_cb status_cb, void *status_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	char *fileindex_path = NULL;
	struct got_fileindex *fileindex = NULL;
	struct got_pathlist_entry *pe;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		return err;

	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
			status_cb, status_arg, cancel_cb, cancel_arg, 0, 0);
		if (err)
			break;
	}
	free(fileindex_path);
	got_fileindex_free(fileindex);
	return err;
}

const struct got_error *
got_worktree_resolve_path(char **wt_path, struct got_worktree *worktree,
    const char *arg)
{
	const struct got_error *err = NULL;
	char *resolved = NULL, *cwd = NULL, *path = NULL;
	size_t len;
	struct stat sb;

	*wt_path = NULL;

	cwd = getcwd(NULL, 0);
	if (cwd == NULL)
		return got_error_from_errno("getcwd");

	if (lstat(arg, &sb) == -1) {
		if (errno != ENOENT) {
			err = got_error_from_errno2("lstat", arg);
			goto done;
		}
	}
	if (S_ISLNK(sb.st_mode)) {
		/*
		 * We cannot use realpath(3) with symlinks since we want to
		 * operate on the symlink itself.
		 * But we can make the path absolute, assuming it is relative
		 * to the current working directory, and then canonicalize it.
		 */
		char *abspath = NULL;
		char canonpath[PATH_MAX];
		if (!got_path_is_absolute(arg)) {
			if (asprintf(&abspath, "%s/%s", cwd, arg) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}

		}
		err = got_canonpath(abspath ? abspath : arg, canonpath,
		    sizeof(canonpath));
		if (err)
			goto done;
		resolved = strdup(canonpath);
		if (resolved == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	} else {
		resolved = realpath(arg, NULL);
		if (resolved == NULL) {
			if (errno != ENOENT) {
				err = got_error_from_errno2("realpath", arg);
				goto done;
			}
			if (asprintf(&resolved, "%s/%s", cwd, arg) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
	}

	if (strncmp(got_worktree_get_root_path(worktree), resolved,
	    strlen(got_worktree_get_root_path(worktree)))) {
		err = got_error_path(resolved, GOT_ERR_BAD_PATH);
		goto done;
	}

	if (strlen(resolved) > strlen(got_worktree_get_root_path(worktree))) {
		err = got_path_skip_common_ancestor(&path,
		    got_worktree_get_root_path(worktree), resolved);
		if (err)
			goto done;
	} else {
		path = strdup("");
		if (path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}

	/* XXX status walk can't deal with trailing slash! */
	len = strlen(path);
	while (len > 0 && path[len - 1] == '/') {
		path[len - 1] = '\0';
		len--;
	}
done:
	free(resolved);
	free(cwd);
	if (err == NULL)
		*wt_path = path;
	else
		free(path);
	return err;
}

struct schedule_addition_args {
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	got_worktree_checkout_cb progress_cb;
	void *progress_arg;
	struct got_repository *repo;
};

static const struct got_error *
schedule_addition(void *arg, unsigned char status, unsigned char staged_status,
    const char *relpath, struct got_object_id *blob_id,
    struct got_object_id *staged_blob_id, struct got_object_id *commit_id,
    int dirfd, const char *de_name)
{
	struct schedule_addition_args *a = arg;
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie;
	struct stat sb;
	char *ondisk_path;

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
	    relpath) == -1)
		return got_error_from_errno("asprintf");

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie) {
		err = get_file_status(&status, &sb, ie, ondisk_path, dirfd,
		    de_name, a->repo);
		if (err)
			goto done;
		/* Re-adding an existing entry is a no-op. */
		if (status == GOT_STATUS_ADD)
			goto done;
		err = got_error_path(relpath, GOT_ERR_FILE_STATUS);
		if (err)
			goto done;
	}

	if (status != GOT_STATUS_UNVERSIONED) {
		err = got_error_path(ondisk_path, GOT_ERR_FILE_STATUS);
		goto done;
	}

	err = got_fileindex_entry_alloc(&ie, relpath);
	if (err)
		goto done;
	err = got_fileindex_entry_update(ie, ondisk_path, NULL, NULL, 1);
	if (err) {
		got_fileindex_entry_free(ie);
		goto done;
	}
	err = got_fileindex_entry_add(a->fileindex, ie);
	if (err) {
		got_fileindex_entry_free(ie);
		goto done;
	}
done:
	free(ondisk_path);
	if (err)
		return err;
	if (status == GOT_STATUS_ADD)
		return NULL;
	return (*a->progress_cb)(a->progress_arg, GOT_STATUS_ADD, relpath);
}

const struct got_error *
got_worktree_schedule_add(struct got_worktree *worktree,
    struct got_pathlist_head *paths,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    struct got_repository *repo, int no_ignores)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;
	struct schedule_addition_args saa;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	saa.worktree = worktree;
	saa.fileindex = fileindex;
	saa.progress_cb = progress_cb;
	saa.progress_arg = progress_arg;
	saa.repo = repo;

	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
			schedule_addition, &saa, NULL, NULL, no_ignores, 0);
		if (err)
			break;
	}
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

struct schedule_deletion_args {
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	got_worktree_delete_cb progress_cb;
	void *progress_arg;
	struct got_repository *repo;
	int delete_local_mods;
	int keep_on_disk;
	const char *status_codes;
};

static const struct got_error *
schedule_for_deletion(void *arg, unsigned char status,
    unsigned char staged_status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *staged_blob_id,
    struct got_object_id *commit_id, int dirfd, const char *de_name)
{
	struct schedule_deletion_args *a = arg;
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie = NULL;
	struct stat sb;
	char *ondisk_path, *parent = NULL;

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie == NULL)
		return got_error_path(relpath, GOT_ERR_BAD_PATH);

	staged_status = get_staged_status(ie);
	if (staged_status != GOT_STATUS_NO_CHANGE) {
		if (staged_status == GOT_STATUS_DELETE)
			return NULL;
		return got_error_path(relpath, GOT_ERR_FILE_STAGED);
	}

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
	    relpath) == -1)
		return got_error_from_errno("asprintf");

	err = get_file_status(&status, &sb, ie, ondisk_path, dirfd, de_name,
	    a->repo);
	if (err)
		goto done;

	if (a->status_codes) {
		size_t ncodes = strlen(a->status_codes);
		int i;
		for (i = 0; i < ncodes ; i++) {
			if (status == a->status_codes[i])
				break;
		}
		if (i == ncodes) { 
			/* Do not delete files in non-matching status. */
			free(ondisk_path);
			return NULL;
		}
		if (a->status_codes[i] != GOT_STATUS_MODIFY &&
		    a->status_codes[i] != GOT_STATUS_MISSING) {
			static char msg[64];
			snprintf(msg, sizeof(msg),
			    "invalid status code '%c'", a->status_codes[i]);
			err = got_error_msg(GOT_ERR_FILE_STATUS, msg);
			goto done;
		}
	}

	if (status != GOT_STATUS_NO_CHANGE) {
		if (status == GOT_STATUS_DELETE)
			goto done;
		if (status == GOT_STATUS_MODIFY && !a->delete_local_mods) {
			err = got_error_path(relpath, GOT_ERR_FILE_MODIFIED);
			goto done;
		}
		if (status != GOT_STATUS_MODIFY &&
		    status != GOT_STATUS_MISSING) {
			err = got_error_path(relpath, GOT_ERR_FILE_STATUS);
			goto done;
		}
	}

	if (!a->keep_on_disk && status != GOT_STATUS_MISSING) {
		if (dirfd != -1) {
			if (unlinkat(dirfd, de_name, 0) != 0) {
				err = got_error_from_errno2("unlinkat",
				    ondisk_path);
				goto done;
			}
		} else if (unlink(ondisk_path) != 0) {
			err = got_error_from_errno2("unlink", ondisk_path);
			goto done;
		}

		parent = dirname(ondisk_path);

		if (parent == NULL) {
			err = got_error_from_errno2("dirname", ondisk_path);
			goto done;
		}
		while (parent && strcmp(parent, a->worktree->root_path) != 0) {
			if (rmdir(parent) == -1) {
				if (errno != ENOTEMPTY)
					err = got_error_from_errno2("rmdir",
					    parent);
				break;
			}
			parent = dirname(parent);
			if (parent == NULL) {
				err = got_error_from_errno2("dirname", parent);
				goto done;
			}
		}
	}

	got_fileindex_entry_mark_deleted_from_disk(ie);
done:
	free(ondisk_path);
	if (err)
		return err;
	if (status == GOT_STATUS_DELETE)
		return NULL;
	return (*a->progress_cb)(a->progress_arg, GOT_STATUS_DELETE,
	    staged_status, relpath);
}

const struct got_error *
got_worktree_schedule_delete(struct got_worktree *worktree,
    struct got_pathlist_head *paths, int delete_local_mods,
    const char *status_codes,
    got_worktree_delete_cb progress_cb, void *progress_arg,
    struct got_repository *repo, int keep_on_disk)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;
	struct schedule_deletion_args sda;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	sda.worktree = worktree;
	sda.fileindex = fileindex;
	sda.progress_cb = progress_cb;
	sda.progress_arg = progress_arg;
	sda.repo = repo;
	sda.delete_local_mods = delete_local_mods;
	sda.keep_on_disk = keep_on_disk;
	sda.status_codes = status_codes;

	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
			schedule_for_deletion, &sda, NULL, NULL, 0, 1);
		if (err)
			break;
	}
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

static const struct got_error *
copy_one_line(FILE *infile, FILE *outfile, FILE *rejectfile)
{
	const struct got_error *err = NULL;
	char *line = NULL;
	size_t linesize = 0, n;
	ssize_t linelen;

	linelen = getline(&line, &linesize, infile);
	if (linelen == -1) {
		if (ferror(infile)) {
			err = got_error_from_errno("getline");
			goto done;
		}
		return NULL;
	}
	if (outfile) {
		n = fwrite(line, 1, linelen, outfile);
		if (n != linelen) {
			err = got_ferror(outfile, GOT_ERR_IO);
			goto done;
		}
	}
	if (rejectfile) {
		n = fwrite(line, 1, linelen, rejectfile);
		if (n != linelen)
			err = got_ferror(outfile, GOT_ERR_IO);
	}
done:
	free(line);
	return err;
}

static const struct got_error *
skip_one_line(FILE *f)
{
	char *line = NULL;
	size_t linesize = 0;
	ssize_t linelen;

	linelen = getline(&line, &linesize, f);
	if (linelen == -1) {
		if (ferror(f))
			return got_error_from_errno("getline");
		return NULL;
	}
	free(line);
	return NULL;
}

static const struct got_error *
copy_change(FILE *f1, FILE *f2, int *line_cur1, int *line_cur2,
    int start_old, int end_old, int start_new, int end_new,
    FILE *outfile, FILE *rejectfile)
 {
	const struct got_error *err;

	/* Copy old file's lines leading up to patch. */
	while (!feof(f1) && *line_cur1 < start_old) {
		err = copy_one_line(f1, outfile, NULL);
		if (err)
			return err;
		(*line_cur1)++;
	}
	/* Skip new file's lines leading up to patch. */
	while (!feof(f2) && *line_cur2 < start_new) {
		if (rejectfile)
			err = copy_one_line(f2, NULL, rejectfile);
		else
			err = skip_one_line(f2);
		if (err)
			return err;
		(*line_cur2)++;
	}
	/* Copy patched lines. */
	while (!feof(f2) && *line_cur2 <= end_new) {
		err = copy_one_line(f2, outfile, NULL);
		if (err)
			return err;
		(*line_cur2)++;
	}
	/* Skip over old file's replaced lines. */
	while (!feof(f1) && *line_cur1 <= end_old) {
		if (rejectfile)
			err = copy_one_line(f1, NULL, rejectfile);
		else
			err = skip_one_line(f1);
		if (err)
			return err;
		(*line_cur1)++;
	}

	return NULL;
}

static const struct got_error *
copy_remaining_content(FILE *f1, FILE *f2, int *line_cur1, int *line_cur2,
    FILE *outfile, FILE *rejectfile)
{
	const struct got_error *err;

	if (outfile) {
		/* Copy old file's lines until EOF. */
		while (!feof(f1)) {
			err = copy_one_line(f1, outfile, NULL);
			if (err)
				return err;
			(*line_cur1)++;
		}
	}
	if (rejectfile) {
		/* Copy new file's lines until EOF. */
		while (!feof(f2)) {
			err = copy_one_line(f2, NULL, rejectfile);
			if (err)
				return err;
			(*line_cur2)++;
		}
	}

	return NULL;
}

static const struct got_error *
apply_or_reject_change(int *choice, struct got_diff_change *change, int n,
    int nchanges, struct got_diff_state *ds, struct got_diff_args *args,
    int diff_flags, const char *relpath, FILE *f1, FILE *f2, int *line_cur1,
    int *line_cur2, FILE *outfile, FILE *rejectfile,
    got_worktree_patch_cb patch_cb, void *patch_arg)
{
	const struct got_error *err = NULL;
	int start_old = change->cv.a;
	int end_old = change->cv.b;
	int start_new = change->cv.c;
	int end_new = change->cv.d;
	long pos1, pos2;
	FILE *hunkfile;

	*choice = GOT_PATCH_CHOICE_NONE;

	hunkfile = got_opentemp();
	if (hunkfile == NULL)
		return got_error_from_errno("got_opentemp");

	pos1 = ftell(f1);
	pos2 = ftell(f2);

	/* XXX TODO needs error checking */
	got_diff_dump_change(hunkfile, change, ds, args, f1, f2, diff_flags);

	if (fseek(f1, pos1, SEEK_SET) == -1) {
		err = got_ferror(f1, GOT_ERR_IO);
		goto done;
	}
	if (fseek(f2, pos2, SEEK_SET) == -1) {
		err = got_ferror(f1, GOT_ERR_IO);
		goto done;
	}
	if (fseek(hunkfile, 0L, SEEK_SET) == -1) {
		err = got_ferror(hunkfile, GOT_ERR_IO);
		goto done;
	}

	err = (*patch_cb)(choice, patch_arg, GOT_STATUS_MODIFY, relpath,
	    hunkfile, n, nchanges);
	if (err)
		goto done;

	switch (*choice) {
	case GOT_PATCH_CHOICE_YES:
		err = copy_change(f1, f2, line_cur1, line_cur2, start_old,
		    end_old, start_new, end_new, outfile, rejectfile);
		break;
	case GOT_PATCH_CHOICE_NO:
		err = copy_change(f1, f2, line_cur1, line_cur2, start_old,
		    end_old, start_new, end_new, rejectfile, outfile);
		break;
	case GOT_PATCH_CHOICE_QUIT:
		break;
	default:
		err = got_error(GOT_ERR_PATCH_CHOICE);
		break;
	}
done:
	if (hunkfile && fclose(hunkfile) == EOF && err == NULL)
		err = got_error_from_errno("fclose");
	return err;
}

struct revert_file_args {
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	got_worktree_checkout_cb progress_cb;
	void *progress_arg;
	got_worktree_patch_cb patch_cb;
	void *patch_arg;
	struct got_repository *repo;
};

static const struct got_error *
create_patched_content(char **path_outfile, int reverse_patch,
    struct got_object_id *blob_id, const char *path2,
    int dirfd2, const char *de_name2,
    const char *relpath, struct got_repository *repo,
    got_worktree_patch_cb patch_cb, void *patch_arg)
{
	const struct got_error *err;
	struct got_blob_object *blob = NULL;
	FILE *f1 = NULL, *f2 = NULL, *outfile = NULL;
	int fd2 = -1;
	char link_target[PATH_MAX];
	ssize_t link_len = 0;
	char *path1 = NULL, *id_str = NULL;
	struct stat sb1, sb2;
	struct got_diff_changes *changes = NULL;
	struct got_diff_state *ds = NULL;
	struct got_diff_args *args = NULL;
	struct got_diff_change *change;
	int diff_flags = 0, line_cur1 = 1, line_cur2 = 1, have_content = 0;
	int n = 0;

	*path_outfile = NULL;

	err = got_object_id_str(&id_str, blob_id);
	if (err)
		return err;

	if (dirfd2 != -1) {
		fd2 = openat(dirfd2, de_name2, O_RDONLY | O_NOFOLLOW);
		if (fd2 == -1) {
			if (errno != ELOOP) {
				err = got_error_from_errno2("openat", path2);
				goto done;
			}
			link_len = readlinkat(dirfd2, de_name2,
			    link_target, sizeof(link_target));
			if (link_len == -1)
				return got_error_from_errno2("readlinkat", path2);
			sb2.st_mode = S_IFLNK;
			sb2.st_size = link_len;
		}
	} else {
		fd2 = open(path2, O_RDONLY | O_NOFOLLOW);
		if (fd2 == -1) {
			if (errno != ELOOP) {
				err = got_error_from_errno2("open", path2);
				goto done;
			}
			link_len = readlink(path2, link_target,
			    sizeof(link_target));
			if (link_len == -1)
				return got_error_from_errno2("readlink", path2);
			sb2.st_mode = S_IFLNK;
			sb2.st_size = link_len;
		}
	}
	if (fd2 != -1) {
		if (fstat(fd2, &sb2) == -1) {
			err = got_error_from_errno2("fstat", path2);
			goto done;
		}

		f2 = fdopen(fd2, "r");
		if (f2 == NULL) {
			err = got_error_from_errno2("fdopen", path2);
			goto done;
		}
		fd2 = -1;
	} else {
		size_t n;
		f2 = got_opentemp();
		if (f2 == NULL) {
			err = got_error_from_errno2("got_opentemp", path2);
			goto done;
		}
		n = fwrite(link_target, 1, link_len, f2);
		if (n != link_len) {
			err = got_ferror(f2, GOT_ERR_IO);
			goto done;
		}
		if (fflush(f2) == EOF) {
			err = got_error_from_errno("fflush");
			goto done;
		}
		rewind(f2);
	}

	err = got_object_open_as_blob(&blob, repo, blob_id, 8192);
	if (err)
		goto done;

	err = got_opentemp_named(&path1, &f1, "got-patched-blob");
	if (err)
		goto done;

	err = got_object_blob_dump_to_file(NULL, NULL, NULL, f1, blob);
	if (err)
		goto done;

	if (stat(path1, &sb1) == -1) {
		err = got_error_from_errno2("stat", path1);
		goto done;
	}

	err = got_diff_files(&changes, &ds, &args, &diff_flags,
	    f1, sb1.st_size, id_str, f2, sb2.st_size, path2, 3, NULL);
	if (err)
		goto done;

	err = got_opentemp_named(path_outfile, &outfile, "got-patched-content");
	if (err)
		goto done;

	if (fseek(f1, 0L, SEEK_SET) == -1)
		return got_ferror(f1, GOT_ERR_IO);
	if (fseek(f2, 0L, SEEK_SET) == -1)
		return got_ferror(f2, GOT_ERR_IO);
	SIMPLEQ_FOREACH(change, &changes->entries, entry) {
		int choice;
		err = apply_or_reject_change(&choice, change, ++n,
		    changes->nchanges, ds, args, diff_flags, relpath,
		    f1, f2, &line_cur1, &line_cur2,
		    reverse_patch ? NULL : outfile,
		    reverse_patch ? outfile : NULL,
		    patch_cb, patch_arg);
		if (err)
			goto done;
		if (choice == GOT_PATCH_CHOICE_YES)
			have_content = 1;
		else if (choice == GOT_PATCH_CHOICE_QUIT)
			break;
	}
	if (have_content) {
		err = copy_remaining_content(f1, f2, &line_cur1, &line_cur2,
		    reverse_patch ? NULL : outfile,
		    reverse_patch ? outfile : NULL);
		if (err)
			goto done;

		if (!S_ISLNK(sb2.st_mode)) {
			if (chmod(*path_outfile, sb2.st_mode) == -1) {
				err = got_error_from_errno2("chmod", path2);
				goto done;
			}
		}
	}
done:
	free(id_str);
	if (blob)
		got_object_blob_close(blob);
	if (f1 && fclose(f1) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path1);
	if (f2 && fclose(f2) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path2);
	if (fd2 != -1 && close(fd2) == -1 && err == NULL)
		err = got_error_from_errno2("close", path2);
	if (outfile && fclose(outfile) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", *path_outfile);
	if (path1 && unlink(path1) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path1);
	if (err || !have_content) {
		if (*path_outfile && unlink(*path_outfile) == -1 && err == NULL)
			err = got_error_from_errno2("unlink", *path_outfile);
		free(*path_outfile);
		*path_outfile = NULL;
	}
	free(args);
	if (ds) {
		got_diff_state_free(ds);
		free(ds);
	}
	if (changes)
		got_diff_free_changes(changes);
	free(path1);
	return err;
}

static const struct got_error *
revert_file(void *arg, unsigned char status, unsigned char staged_status,
    const char *relpath, struct got_object_id *blob_id,
    struct got_object_id *staged_blob_id, struct got_object_id *commit_id,
    int dirfd, const char *de_name)
{
	struct revert_file_args *a = arg;
	const struct got_error *err = NULL;
	char *parent_path = NULL;
	struct got_fileindex_entry *ie;
	struct got_tree_object *tree = NULL;
	struct got_object_id *tree_id = NULL;
	const struct got_tree_entry *te = NULL;
	char *tree_path = NULL, *te_name;
	char *ondisk_path = NULL, *path_content = NULL;
	struct got_blob_object *blob = NULL;

	/* Reverting a staged deletion is a no-op. */
	if (status == GOT_STATUS_DELETE &&
	    staged_status != GOT_STATUS_NO_CHANGE)
		return NULL;

	if (status == GOT_STATUS_UNVERSIONED)
		return (*a->progress_cb)(a->progress_arg,
		    GOT_STATUS_UNVERSIONED, relpath);

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie == NULL)
		return got_error_path(relpath, GOT_ERR_BAD_PATH);

	/* Construct in-repository path of tree which contains this blob. */
	err = got_path_dirname(&parent_path, ie->path);
	if (err) {
		if (err->code != GOT_ERR_BAD_PATH)
			goto done;
		parent_path = strdup("/");
		if (parent_path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}
	if (got_path_is_root_dir(a->worktree->path_prefix)) {
		tree_path = strdup(parent_path);
		if (tree_path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	} else {
		if (got_path_is_root_dir(parent_path)) {
			tree_path = strdup(a->worktree->path_prefix);
			if (tree_path == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		} else {
			if (asprintf(&tree_path, "%s/%s",
			    a->worktree->path_prefix, parent_path) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
	}

	err = got_object_id_by_path(&tree_id, a->repo,
	    a->worktree->base_commit_id, tree_path);
	if (err) {
		if (!(err->code == GOT_ERR_NO_TREE_ENTRY &&
		    (status == GOT_STATUS_ADD ||
		    staged_status == GOT_STATUS_ADD)))
			goto done;
	} else {
		err = got_object_open_as_tree(&tree, a->repo, tree_id);
		if (err)
			goto done;

		te_name = basename(ie->path);
		if (te_name == NULL) {
			err = got_error_from_errno2("basename", ie->path);
			goto done;
		}

		te = got_object_tree_find_entry(tree, te_name);
		if (te == NULL && status != GOT_STATUS_ADD &&
		    staged_status != GOT_STATUS_ADD) {
			err = got_error_path(ie->path, GOT_ERR_NO_TREE_ENTRY);
			goto done;
		}
	}

	switch (status) {
	case GOT_STATUS_ADD:
		if (a->patch_cb) {
			int choice = GOT_PATCH_CHOICE_NONE;
			err = (*a->patch_cb)(&choice, a->patch_arg,
			    status, ie->path, NULL, 1, 1);
			if (err)
				goto done;
			if (choice != GOT_PATCH_CHOICE_YES)
				break;
		}
		err = (*a->progress_cb)(a->progress_arg, GOT_STATUS_REVERT,
		    ie->path);
		if (err)
			goto done;
		got_fileindex_entry_remove(a->fileindex, ie);
		break;
	case GOT_STATUS_DELETE:
		if (a->patch_cb) {
			int choice = GOT_PATCH_CHOICE_NONE;
			err = (*a->patch_cb)(&choice, a->patch_arg,
			    status, ie->path, NULL, 1, 1);
			if (err)
				goto done;
			if (choice != GOT_PATCH_CHOICE_YES)
				break;
		}
		/* fall through */
	case GOT_STATUS_MODIFY:
	case GOT_STATUS_MODE_CHANGE:
	case GOT_STATUS_CONFLICT:
	case GOT_STATUS_MISSING: {
		struct got_object_id id;
		if (staged_status == GOT_STATUS_ADD ||
		    staged_status == GOT_STATUS_MODIFY) {
			memcpy(id.sha1, ie->staged_blob_sha1,
			    SHA1_DIGEST_LENGTH);
		} else
			memcpy(id.sha1, ie->blob_sha1,
			    SHA1_DIGEST_LENGTH);
		err = got_object_open_as_blob(&blob, a->repo, &id, 8192);
		if (err)
			goto done;

		if (asprintf(&ondisk_path, "%s/%s",
		    got_worktree_get_root_path(a->worktree), relpath) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}

		if (a->patch_cb && (status == GOT_STATUS_MODIFY ||
		    status == GOT_STATUS_CONFLICT)) {
			int is_bad_symlink = 0;
			err = create_patched_content(&path_content, 1, &id,
			    ondisk_path, dirfd, de_name, ie->path, a->repo,
			    a->patch_cb, a->patch_arg);
			if (err || path_content == NULL)
				break;
			if (te && S_ISLNK(te->mode)) {
				if (unlink(path_content) == -1) {
					err = got_error_from_errno2("unlink",
					    path_content);
					break;
				}
				err = install_symlink(&is_bad_symlink,
				    a->worktree, ondisk_path, ie->path,
				    blob, 0, 1, 0, a->repo,
				    a->progress_cb, a->progress_arg);
			} else {
				if (rename(path_content, ondisk_path) == -1) {
					err = got_error_from_errno3("rename",
					    path_content, ondisk_path);
					goto done;
				}
			}
		} else {
			int is_bad_symlink = 0;
			if (te && S_ISLNK(te->mode)) {
				err = install_symlink(&is_bad_symlink,
				    a->worktree, ondisk_path, ie->path,
				    blob, 0, 1, 0, a->repo,
				    a->progress_cb, a->progress_arg);
			} else {
				err = install_blob(a->worktree, ondisk_path,
				    ie->path,
				    te ? te->mode : GOT_DEFAULT_FILE_MODE,
				    got_fileindex_perms_to_st(ie), blob,
				    0, 1, 0, 0, a->repo,
				    a->progress_cb, a->progress_arg);
			}
			if (err)
				goto done;
			if (status == GOT_STATUS_DELETE ||
			    status == GOT_STATUS_MODE_CHANGE) {
				err = got_fileindex_entry_update(ie,
				    ondisk_path, blob->id.sha1,
				    a->worktree->base_commit_id->sha1, 1);
				if (err)
					goto done;
			}
			if (is_bad_symlink) {
				got_fileindex_entry_filetype_set(ie,
				    GOT_FILEIDX_MODE_BAD_SYMLINK);
			}
		}
		break;
	}
	default:
		break;
	}
done:
	free(ondisk_path);
	free(path_content);
	free(parent_path);
	free(tree_path);
	if (blob)
		got_object_blob_close(blob);
	if (tree)
		got_object_tree_close(tree);
	free(tree_id);
	return err;
}

const struct got_error *
got_worktree_revert(struct got_worktree *worktree,
    struct got_pathlist_head *paths,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_worktree_patch_cb patch_cb, void *patch_arg,
    struct got_repository *repo)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *unlockerr = NULL;
	const struct got_error *sync_err = NULL;
	struct got_pathlist_entry *pe;
	struct revert_file_args rfa;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	rfa.worktree = worktree;
	rfa.fileindex = fileindex;
	rfa.progress_cb = progress_cb;
	rfa.progress_arg = progress_arg;
	rfa.patch_cb = patch_cb;
	rfa.patch_arg = patch_arg;
	rfa.repo = repo;
	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
		    revert_file, &rfa, NULL, NULL, 0, 0);
		if (err)
			break;
	}
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

static void
free_commitable(struct got_commitable *ct)
{
	free(ct->path);
	free(ct->in_repo_path);
	free(ct->ondisk_path);
	free(ct->blob_id);
	free(ct->base_blob_id);
	free(ct->staged_blob_id);
	free(ct->base_commit_id);
	free(ct);
}

struct collect_commitables_arg {
	struct got_pathlist_head *commitable_paths;
	struct got_repository *repo;
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	int have_staged_files;
	int allow_bad_symlinks;
};

static const struct got_error *
collect_commitables(void *arg, unsigned char status,
    unsigned char staged_status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *staged_blob_id,
    struct got_object_id *commit_id, int dirfd, const char *de_name)
{
	struct collect_commitables_arg *a = arg;
	const struct got_error *err = NULL;
	struct got_commitable *ct = NULL;
	struct got_pathlist_entry *new = NULL;
	char *parent_path = NULL, *path = NULL;
	struct stat sb;

	if (a->have_staged_files) {
		if (staged_status != GOT_STATUS_MODIFY &&
		    staged_status != GOT_STATUS_ADD &&
		    staged_status != GOT_STATUS_DELETE)
			return NULL;
	} else {
		if (status == GOT_STATUS_CONFLICT)
			return got_error(GOT_ERR_COMMIT_CONFLICT);

		if (status != GOT_STATUS_MODIFY &&
		    status != GOT_STATUS_MODE_CHANGE &&
		    status != GOT_STATUS_ADD &&
		    status != GOT_STATUS_DELETE)
			return NULL;
	}

	if (asprintf(&path, "/%s", relpath) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}
	if (strcmp(path, "/") == 0) {
		parent_path = strdup("");
		if (parent_path == NULL)
			return got_error_from_errno("strdup");
	} else {
		err = got_path_dirname(&parent_path, path);
		if (err)
			return err;
	}

	ct = calloc(1, sizeof(*ct));
	if (ct == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	if (asprintf(&ct->ondisk_path, "%s/%s", a->worktree->root_path,
	    relpath) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	if (staged_status == GOT_STATUS_ADD ||
	    staged_status == GOT_STATUS_MODIFY) {
		struct got_fileindex_entry *ie;
		ie = got_fileindex_entry_get(a->fileindex, path, strlen(path));
		switch (got_fileindex_entry_staged_filetype_get(ie)) {
		case GOT_FILEIDX_MODE_REGULAR_FILE:
		case GOT_FILEIDX_MODE_BAD_SYMLINK:
			ct->mode = S_IFREG;
			break;
		case GOT_FILEIDX_MODE_SYMLINK:
			ct->mode = S_IFLNK;
			break;
		default:
			err = got_error_path(path, GOT_ERR_BAD_FILETYPE);
			goto done;
		}
		ct->mode |= got_fileindex_entry_perms_get(ie);
	} else if (status != GOT_STATUS_DELETE &&
	    staged_status != GOT_STATUS_DELETE) {
		if (dirfd != -1) {
			if (fstatat(dirfd, de_name, &sb,
			    AT_SYMLINK_NOFOLLOW) == -1) {
				err = got_error_from_errno2("fstatat",
				    ct->ondisk_path);
				goto done;
			}
		} else if (lstat(ct->ondisk_path, &sb) == -1) {
			err = got_error_from_errno2("lstat", ct->ondisk_path);
			goto done;
		}
		ct->mode = sb.st_mode;
	}

	if (asprintf(&ct->in_repo_path, "%s%s%s", a->worktree->path_prefix,
	    got_path_is_root_dir(a->worktree->path_prefix) ? "" : "/",
	    relpath) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	if (S_ISLNK(ct->mode) && staged_status == GOT_STATUS_NO_CHANGE &&
	    status == GOT_STATUS_ADD && !a->allow_bad_symlinks) {
		int is_bad_symlink;
		char target_path[PATH_MAX];
		ssize_t target_len;
		target_len = readlink(ct->ondisk_path, target_path,
		    sizeof(target_path));
		if (target_len == -1) {
			err = got_error_from_errno2("readlink",
			    ct->ondisk_path);
			goto done;
		}
		err = is_bad_symlink_target(&is_bad_symlink, target_path,
		    target_len, ct->ondisk_path, a->worktree->root_path);
		if (err)
			goto done;
		if (is_bad_symlink) {
			err = got_error_path(ct->ondisk_path,
			    GOT_ERR_BAD_SYMLINK);
			goto done;
		}
	}


	ct->status = status;
	ct->staged_status = staged_status;
	ct->blob_id = NULL; /* will be filled in when blob gets created */
	if (ct->status != GOT_STATUS_ADD &&
	    ct->staged_status != GOT_STATUS_ADD) {
		ct->base_blob_id = got_object_id_dup(blob_id);
		if (ct->base_blob_id == NULL) {
			err = got_error_from_errno("got_object_id_dup");
			goto done;
		}
		ct->base_commit_id = got_object_id_dup(commit_id);
		if (ct->base_commit_id == NULL) {
			err = got_error_from_errno("got_object_id_dup");
			goto done;
		}
	}
	if (ct->staged_status == GOT_STATUS_ADD ||
	    ct->staged_status == GOT_STATUS_MODIFY) {
		ct->staged_blob_id = got_object_id_dup(staged_blob_id);
		if (ct->staged_blob_id == NULL) {
			err = got_error_from_errno("got_object_id_dup");
			goto done;
		}
	}
	ct->path = strdup(path);
	if (ct->path == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}
	err = got_pathlist_insert(&new, a->commitable_paths, ct->path, ct);
done:
	if (ct && (err || new == NULL))
		free_commitable(ct);
	free(parent_path);
	free(path);
	return err;
}

static const struct got_error *write_tree(struct got_object_id **, int *,
    struct got_tree_object *, const char *, struct got_pathlist_head *,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *);

static const struct got_error *
write_subtree(struct got_object_id **new_subtree_id, int *nentries,
    struct got_tree_entry *te, const char *parent_path,
    struct got_pathlist_head *commitable_paths,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_tree_object *subtree;
	char *subpath;

	if (asprintf(&subpath, "%s%s%s", parent_path,
	    got_path_is_root_dir(parent_path) ? "" : "/", te->name) == -1)
		return got_error_from_errno("asprintf");

	err = got_object_open_as_tree(&subtree, repo, &te->id);
	if (err)
		return err;

	err = write_tree(new_subtree_id, nentries, subtree, subpath,
	    commitable_paths, status_cb, status_arg, repo);
	got_object_tree_close(subtree);
	free(subpath);
	return err;
}

static const struct got_error *
match_ct_parent_path(int *match, struct got_commitable *ct, const char *path)
{
	const struct got_error *err = NULL;
	char *ct_parent_path = NULL;

	*match = 0;

	if (strchr(ct->in_repo_path, '/') == NULL) {
		*match = got_path_is_root_dir(path);
		return NULL;
	}

	err = got_path_dirname(&ct_parent_path, ct->in_repo_path);
	if (err)
		return err;
	*match = (strcmp(path, ct_parent_path) == 0);
	free(ct_parent_path);
	return err;
}

static mode_t
get_ct_file_mode(struct got_commitable *ct)
{
	if (S_ISLNK(ct->mode))
		return S_IFLNK;

	return S_IFREG | (ct->mode & ((S_IRWXU | S_IRWXG | S_IRWXO)));
}

static const struct got_error *
alloc_modified_blob_tree_entry(struct got_tree_entry **new_te,
    struct got_tree_entry *te, struct got_commitable *ct)
{
	const struct got_error *err = NULL;

	*new_te = NULL;

	err = got_object_tree_entry_dup(new_te, te);
	if (err)
		goto done;

	(*new_te)->mode = get_ct_file_mode(ct);

	if (ct->staged_status == GOT_STATUS_MODIFY)
		memcpy(&(*new_te)->id, ct->staged_blob_id,
		    sizeof((*new_te)->id));
	else
		memcpy(&(*new_te)->id, ct->blob_id, sizeof((*new_te)->id));
done:
	if (err && *new_te) {
		free(*new_te);
		*new_te = NULL;
	}
	return err;
}

static const struct got_error *
alloc_added_blob_tree_entry(struct got_tree_entry **new_te,
    struct got_commitable *ct)
{
	const struct got_error *err = NULL;
	char *ct_name;

	 *new_te = NULL;

	*new_te = calloc(1, sizeof(**new_te));
	if (*new_te == NULL)
		return got_error_from_errno("calloc");

	ct_name = basename(ct->path);
	if (ct_name == NULL) {
		err = got_error_from_errno2("basename", ct->path);
		goto done;
	}
	if (strlcpy((*new_te)->name, ct_name, sizeof((*new_te)->name)) >=
	    sizeof((*new_te)->name)) {
		err = got_error(GOT_ERR_NO_SPACE);
		goto done;
	}

	(*new_te)->mode = get_ct_file_mode(ct);

	if (ct->staged_status == GOT_STATUS_ADD)
		memcpy(&(*new_te)->id, ct->staged_blob_id,
		    sizeof((*new_te)->id));
	else
		memcpy(&(*new_te)->id, ct->blob_id, sizeof((*new_te)->id));
done:
	if (err && *new_te) {
		free(*new_te);
		*new_te = NULL;
	}
	return err;
}

static const struct got_error *
insert_tree_entry(struct got_tree_entry *new_te,
    struct got_pathlist_head *paths)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *new_pe;

	err = got_pathlist_insert(&new_pe, paths, new_te->name, new_te);
	if (err)
		return err;
	if (new_pe == NULL)
		return got_error(GOT_ERR_TREE_DUP_ENTRY);
	return NULL;
}

static const struct got_error *
report_ct_status(struct got_commitable *ct,
    got_worktree_status_cb status_cb, void *status_arg)
{
	const char *ct_path = ct->path;
	unsigned char status;

	while (ct_path[0] == '/')
		ct_path++;

	if (ct->staged_status != GOT_STATUS_NO_CHANGE)
		status = ct->staged_status;
	else
		status = ct->status;

	return (*status_cb)(status_arg, status, GOT_STATUS_NO_CHANGE,
	    ct_path, ct->blob_id, NULL, NULL, -1, NULL);
}

static const struct got_error *
match_modified_subtree(int *modified, struct got_tree_entry *te,
    const char *base_tree_path, struct got_pathlist_head *commitable_paths)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;
	char *te_path;

	*modified = 0;

	if (asprintf(&te_path, "%s%s%s", base_tree_path,
	    got_path_is_root_dir(base_tree_path) ? "" : "/",
	    te->name) == -1)
		return got_error_from_errno("asprintf");

	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		*modified = got_path_is_child(ct->in_repo_path, te_path,
		    strlen(te_path));
		 if (*modified)
			break;
	}

	free(te_path);
	return err;
}

static const struct got_error *
match_deleted_or_modified_ct(struct got_commitable **ctp,
    struct got_tree_entry *te, const char *base_tree_path,
    struct got_pathlist_head *commitable_paths)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;

	*ctp = NULL;

	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		char *ct_name = NULL;
		int path_matches;

		if (ct->staged_status == GOT_STATUS_NO_CHANGE) {
			if (ct->status != GOT_STATUS_MODIFY &&
			    ct->status != GOT_STATUS_MODE_CHANGE &&
			    ct->status != GOT_STATUS_DELETE)
				continue;
		} else {
			if (ct->staged_status != GOT_STATUS_MODIFY &&
			    ct->staged_status != GOT_STATUS_DELETE)
				continue;
		}

		if (got_object_id_cmp(ct->base_blob_id, &te->id) != 0)
			continue;

		 err = match_ct_parent_path(&path_matches, ct, base_tree_path);
		 if (err)
			return err;
		if (!path_matches)
			continue;

		ct_name = basename(pe->path);
		if (ct_name == NULL)
			return got_error_from_errno2("basename", pe->path);

		if (strcmp(te->name, ct_name) != 0)
			continue;

		*ctp = ct;
		break;
	}

	return err;
}

static const struct got_error *
make_subtree_for_added_blob(struct got_tree_entry **new_tep,
    const char *child_path, const char *path_base_tree,
    struct got_pathlist_head *commitable_paths,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_tree_entry *new_te;
	char *subtree_path;
	struct got_object_id *id = NULL;
	int nentries;

	*new_tep = NULL;

	if (asprintf(&subtree_path, "%s%s%s", path_base_tree,
	    got_path_is_root_dir(path_base_tree) ? "" : "/",
	    child_path) == -1)
		return got_error_from_errno("asprintf");

	new_te = calloc(1, sizeof(*new_te));
	if (new_te == NULL)
		return got_error_from_errno("calloc");
	new_te->mode = S_IFDIR;

	if (strlcpy(new_te->name, child_path, sizeof(new_te->name)) >=
	    sizeof(new_te->name)) {
		err = got_error(GOT_ERR_NO_SPACE);
		goto done;
	}
	err = write_tree(&id, &nentries, NULL, subtree_path,
	    commitable_paths, status_cb, status_arg, repo);
	if (err) {
		free(new_te);
		goto done;
	}
	memcpy(&new_te->id, id, sizeof(new_te->id));
done:
	free(id);
	free(subtree_path);
	if (err == NULL)
		*new_tep = new_te;
	return err;
}

static const struct got_error *
write_tree(struct got_object_id **new_tree_id, int *nentries,
    struct got_tree_object *base_tree, const char *path_base_tree,
    struct got_pathlist_head *commitable_paths,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_pathlist_head paths;
	struct got_tree_entry *te, *new_te = NULL;
	struct got_pathlist_entry *pe;

	TAILQ_INIT(&paths);
	*nentries = 0;

	/* Insert, and recurse into, newly added entries first. */
	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		char *child_path = NULL, *slash;

		if ((ct->status != GOT_STATUS_ADD &&
		    ct->staged_status != GOT_STATUS_ADD) ||
		    (ct->flags & GOT_COMMITABLE_ADDED))
			continue;

		 if (!got_path_is_child(ct->in_repo_path, path_base_tree,
		     strlen(path_base_tree)))
			continue;

		err = got_path_skip_common_ancestor(&child_path, path_base_tree,
		    ct->in_repo_path);
		if (err)
			goto done;

		slash = strchr(child_path, '/');
		if (slash == NULL) {
			err = alloc_added_blob_tree_entry(&new_te, ct);
			if (err)
				goto done;
			err = report_ct_status(ct, status_cb, status_arg);
			if (err)
				goto done;
			ct->flags |= GOT_COMMITABLE_ADDED;
			err = insert_tree_entry(new_te, &paths);
			if (err)
				goto done;
			(*nentries)++;
		} else {
			*slash = '\0'; /* trim trailing path components */
			if (base_tree == NULL ||
			    got_object_tree_find_entry(base_tree, child_path)
			    == NULL) {
				err = make_subtree_for_added_blob(&new_te,
				    child_path, path_base_tree,
				    commitable_paths, status_cb, status_arg,
				    repo);
				if (err)
					goto done;
				err = insert_tree_entry(new_te, &paths);
				if (err)
					goto done;
				(*nentries)++;
			}
		}
	}

	if (base_tree) {
		int i, nbase_entries;
		/* Handle modified and deleted entries. */
		nbase_entries = got_object_tree_get_nentries(base_tree);
		for (i = 0; i < nbase_entries; i++) {
			struct got_commitable *ct = NULL;

			te = got_object_tree_get_entry(base_tree, i);
			if (got_object_tree_entry_is_submodule(te)) {
				/* Entry is a submodule; just copy it. */
				err = got_object_tree_entry_dup(&new_te, te);
				if (err)
					goto done;
				err = insert_tree_entry(new_te, &paths);
				if (err)
					goto done;
				(*nentries)++;
				continue;
			}

			if (S_ISDIR(te->mode)) {
				int modified;
				err = got_object_tree_entry_dup(&new_te, te);
				if (err)
					goto done;
				err = match_modified_subtree(&modified, te,
				    path_base_tree, commitable_paths);
				if (err)
					goto done;
				/* Avoid recursion into unmodified subtrees. */
				if (modified) {
					struct got_object_id *new_id;
					int nsubentries;
					err = write_subtree(&new_id,
					    &nsubentries, te,
					    path_base_tree, commitable_paths,
					    status_cb, status_arg, repo);
					if (err)
						goto done;
					if (nsubentries == 0) {
						/* All entries were deleted. */
						free(new_id);
						continue;
					}
					memcpy(&new_te->id, new_id,
					    sizeof(new_te->id));
					free(new_id);
				}
				err = insert_tree_entry(new_te, &paths);
				if (err)
					goto done;
				(*nentries)++;
				continue;
			}

			err = match_deleted_or_modified_ct(&ct, te,
			    path_base_tree, commitable_paths);
			if (err)
				goto done;
			if (ct) {
				/* NB: Deleted entries get dropped here. */
				if (ct->status == GOT_STATUS_MODIFY ||
				    ct->status == GOT_STATUS_MODE_CHANGE ||
				    ct->staged_status == GOT_STATUS_MODIFY) {
					err = alloc_modified_blob_tree_entry(
					    &new_te, te, ct);
					if (err)
						goto done;
					err = insert_tree_entry(new_te, &paths);
					if (err)
						goto done;
					(*nentries)++;
				}
				err = report_ct_status(ct, status_cb,
				    status_arg);
				if (err)
					goto done;
			} else {
				/* Entry is unchanged; just copy it. */
				err = got_object_tree_entry_dup(&new_te, te);
				if (err)
					goto done;
				err = insert_tree_entry(new_te, &paths);
				if (err)
					goto done;
				(*nentries)++;
			}
		}
	}

	/* Write new list of entries; deleted entries have been dropped. */
	err = got_object_tree_create(new_tree_id, &paths, *nentries, repo);
done:
	got_pathlist_free(&paths);
	return err;
}

static const struct got_error *
update_fileindex_after_commit(struct got_pathlist_head *commitable_paths,
    struct got_object_id *new_base_commit_id, struct got_fileindex *fileindex,
    int have_staged_files)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;

	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_fileindex_entry *ie;
		struct got_commitable *ct = pe->data;

		ie = got_fileindex_entry_get(fileindex, pe->path, pe->path_len);
		if (ie) {
			if (ct->status == GOT_STATUS_DELETE ||
			    ct->staged_status == GOT_STATUS_DELETE) {
				got_fileindex_entry_remove(fileindex, ie);
			} else if (ct->staged_status == GOT_STATUS_ADD ||
			    ct->staged_status == GOT_STATUS_MODIFY) {
				got_fileindex_entry_stage_set(ie,
				    GOT_FILEIDX_STAGE_NONE);
				err = got_fileindex_entry_update(ie,
				    ct->ondisk_path, ct->staged_blob_id->sha1,
				    new_base_commit_id->sha1,
				    !have_staged_files);
			} else
				err = got_fileindex_entry_update(ie,
				    ct->ondisk_path, ct->blob_id->sha1,
				    new_base_commit_id->sha1,
				    !have_staged_files);
		} else {
			err = got_fileindex_entry_alloc(&ie, pe->path);
			if (err)
				break;
			err = got_fileindex_entry_update(ie, ct->ondisk_path,
			    ct->blob_id->sha1, new_base_commit_id->sha1, 1);
			if (err) {
				got_fileindex_entry_free(ie);
				break;
			}
			err = got_fileindex_entry_add(fileindex, ie);
			if (err) {
				got_fileindex_entry_free(ie);
				break;
			}
		}
	}
	return err;
}


static const struct got_error *
check_out_of_date(const char *in_repo_path, unsigned char status,
    unsigned char staged_status, struct got_object_id *base_blob_id,
    struct got_object_id *base_commit_id,
    struct got_object_id *head_commit_id, struct got_repository *repo,
    int ood_errcode)
{
	const struct got_error *err = NULL;
	struct got_object_id *id = NULL;

	if (status != GOT_STATUS_ADD && staged_status != GOT_STATUS_ADD) {
		/* Trivial case: base commit == head commit */
		if (got_object_id_cmp(base_commit_id, head_commit_id) == 0)
			return NULL;
		/*
		 * Ensure file content which local changes were based
		 * on matches file content in the branch head.
		 */
		err = got_object_id_by_path(&id, repo, head_commit_id,
		    in_repo_path);
		if (err) {
			if (err->code == GOT_ERR_NO_TREE_ENTRY)
				err = got_error(ood_errcode);
			goto done;
		} else if (got_object_id_cmp(id, base_blob_id) != 0)
			err = got_error(ood_errcode);
	} else {
		/* Require that added files don't exist in the branch head. */
		err = got_object_id_by_path(&id, repo, head_commit_id,
		    in_repo_path);
		if (err && err->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
		err = id ? got_error(ood_errcode) : NULL;
	}
done:
	free(id);
	return err;
}

const struct got_error *
commit_worktree(struct got_object_id **new_commit_id,
    struct got_pathlist_head *commitable_paths,
    struct got_object_id *head_commit_id, struct got_worktree *worktree,
    const char *author, const char *committer,
    got_worktree_commit_msg_cb commit_msg_cb, void *commit_arg,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL, *unlockerr = NULL;
	struct got_pathlist_entry *pe;
	const char *head_ref_name = NULL;
	struct got_commit_object *head_commit = NULL;
	struct got_reference *head_ref2 = NULL;
	struct got_object_id *head_commit_id2 = NULL;
	struct got_tree_object *head_tree = NULL;
	struct got_object_id *new_tree_id = NULL;
	int nentries;
	struct got_object_id_queue parent_ids;
	struct got_object_qid *pid = NULL;
	char *logmsg = NULL;

	*new_commit_id = NULL;

	SIMPLEQ_INIT(&parent_ids);

	err = got_object_open_as_commit(&head_commit, repo, head_commit_id);
	if (err)
		goto done;

	err = got_object_open_as_tree(&head_tree, repo, head_commit->tree_id);
	if (err)
		goto done;

	if (commit_msg_cb != NULL) {
		err = commit_msg_cb(commitable_paths, &logmsg, commit_arg);
		if (err)
			goto done;
	}

	if (logmsg == NULL || strlen(logmsg) == 0) {
		err = got_error(GOT_ERR_COMMIT_MSG_EMPTY);
		goto done;
	}

	/* Create blobs from added and modified files and record their IDs. */
	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		char *ondisk_path;

		/* Blobs for staged files already exist. */
		if (ct->staged_status == GOT_STATUS_ADD ||
		    ct->staged_status == GOT_STATUS_MODIFY)
			continue;

		if (ct->status != GOT_STATUS_ADD &&
		    ct->status != GOT_STATUS_MODIFY &&
		    ct->status != GOT_STATUS_MODE_CHANGE)
			continue;

		if (asprintf(&ondisk_path, "%s/%s",
		    worktree->root_path, pe->path) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		err = got_object_blob_create(&ct->blob_id, ondisk_path, repo);
		free(ondisk_path);
		if (err)
			goto done;
	}

	/* Recursively write new tree objects. */
	err = write_tree(&new_tree_id, &nentries, head_tree, "/",
	    commitable_paths, status_cb, status_arg, repo);
	if (err)
		goto done;

	err = got_object_qid_alloc(&pid, worktree->base_commit_id);
	if (err)
		goto done;
	SIMPLEQ_INSERT_TAIL(&parent_ids, pid, entry);
	err = got_object_commit_create(new_commit_id, new_tree_id, &parent_ids,
	    1, author, time(NULL), committer, time(NULL), logmsg, repo);
	got_object_qid_free(pid);
	if (logmsg != NULL)
		free(logmsg);
	if (err)
		goto done;

	/* Check if a concurrent commit to our branch has occurred. */
	head_ref_name = got_worktree_get_head_ref_name(worktree);
	if (head_ref_name == NULL) {
		err = got_error_from_errno("got_worktree_get_head_ref_name");
		goto done;
	}
	/* Lock the reference here to prevent concurrent modification. */
	err = got_ref_open(&head_ref2, repo, head_ref_name, 1);
	if (err)
		goto done;
	err = got_ref_resolve(&head_commit_id2, repo, head_ref2);
	if (err)
		goto done;
	if (got_object_id_cmp(head_commit_id, head_commit_id2) != 0) {
		err = got_error(GOT_ERR_COMMIT_HEAD_CHANGED);
		goto done;
	}
	/* Update branch head in repository. */
	err = got_ref_change_ref(head_ref2, *new_commit_id);
	if (err)
		goto done;
	err = got_ref_write(head_ref2, repo);
	if (err)
		goto done;

	err = got_worktree_set_base_commit_id(worktree, repo, *new_commit_id);
	if (err)
		goto done;

	err = ref_base_commit(worktree, repo);
	if (err)
		goto done;
done:
	if (head_tree)
		got_object_tree_close(head_tree);
	if (head_commit)
		got_object_commit_close(head_commit);
	free(head_commit_id2);
	if (head_ref2) {
		unlockerr = got_ref_unlock(head_ref2);
		if (unlockerr && err == NULL)
			err = unlockerr;
		got_ref_close(head_ref2);
	}
	return err;
}

static const struct got_error *
check_path_is_commitable(const char *path,
    struct got_pathlist_head *commitable_paths)
{
	struct got_pathlist_entry *cpe = NULL;
	size_t path_len = strlen(path);

	TAILQ_FOREACH(cpe, commitable_paths, entry) {
		struct got_commitable *ct = cpe->data;
		const char *ct_path = ct->path;

		while (ct_path[0] == '/')
			ct_path++;

		if (strcmp(path, ct_path) == 0 ||
		    got_path_is_child(ct_path, path, path_len))
			break;
	}

	if (cpe == NULL)
		return got_error_path(path, GOT_ERR_BAD_PATH);

	return NULL;
}

static const struct got_error *
check_staged_file(void *arg, struct got_fileindex_entry *ie)
{
	int *have_staged_files = arg;

	if (got_fileindex_entry_stage_get(ie) != GOT_FILEIDX_STAGE_NONE) {
		*have_staged_files = 1;
		return got_error(GOT_ERR_CANCELLED);
	}

	return NULL;
}

static const struct got_error *
check_non_staged_files(struct got_fileindex *fileindex,
    struct got_pathlist_head *paths)
{
	struct got_pathlist_entry *pe;
	struct got_fileindex_entry *ie;

	TAILQ_FOREACH(pe, paths, entry) {
		if (pe->path[0] == '\0')
			continue;
		ie = got_fileindex_entry_get(fileindex, pe->path, pe->path_len);
		if (ie == NULL)
			return got_error_path(pe->path, GOT_ERR_BAD_PATH);
		if (got_fileindex_entry_stage_get(ie) == GOT_FILEIDX_STAGE_NONE)
			return got_error_path(pe->path,
			    GOT_ERR_FILE_NOT_STAGED);
	}

	return NULL;
}

const struct got_error *
got_worktree_commit(struct got_object_id **new_commit_id,
    struct got_worktree *worktree, struct got_pathlist_head *paths,
    const char *author, const char *committer, int allow_bad_symlinks,
    got_worktree_commit_msg_cb commit_msg_cb, void *commit_arg,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL, *unlockerr = NULL, *sync_err;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	struct got_pathlist_head commitable_paths;
	struct collect_commitables_arg cc_arg;
	struct got_pathlist_entry *pe;
	struct got_reference *head_ref = NULL;
	struct got_object_id *head_commit_id = NULL;
	int have_staged_files = 0;

	*new_commit_id = NULL;

	TAILQ_INIT(&commitable_paths);

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		goto done;

	err = got_ref_open(&head_ref, repo, worktree->head_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_resolve(&head_commit_id, repo, head_ref);
	if (err)
		goto done;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	err = got_fileindex_for_each_entry_safe(fileindex, check_staged_file,
	    &have_staged_files);
	if (err && err->code != GOT_ERR_CANCELLED)
		goto done;
	if (have_staged_files) {
		err = check_non_staged_files(fileindex, paths);
		if (err)
			goto done;
	}

	cc_arg.commitable_paths = &commitable_paths;
	cc_arg.worktree = worktree;
	cc_arg.fileindex = fileindex;
	cc_arg.repo = repo;
	cc_arg.have_staged_files = have_staged_files;
	cc_arg.allow_bad_symlinks = allow_bad_symlinks;
	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
		    collect_commitables, &cc_arg, NULL, NULL, 0, 0);
		if (err)
			goto done;
	}

	if (TAILQ_EMPTY(&commitable_paths)) {
		err = got_error(GOT_ERR_COMMIT_NO_CHANGES);
		goto done;
	}

	TAILQ_FOREACH(pe, paths, entry) {
		err = check_path_is_commitable(pe->path, &commitable_paths);
		if (err)
			goto done;
	}

	TAILQ_FOREACH(pe, &commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		const char *ct_path = ct->in_repo_path;

		while (ct_path[0] == '/')
			ct_path++;
		err = check_out_of_date(ct_path, ct->status,
		    ct->staged_status, ct->base_blob_id, ct->base_commit_id,
		    head_commit_id, repo, GOT_ERR_COMMIT_OUT_OF_DATE);
		if (err)
			goto done;

	}

	err = commit_worktree(new_commit_id, &commitable_paths,
	    head_commit_id, worktree, author, committer,
	    commit_msg_cb, commit_arg, status_cb, status_arg, repo);
	if (err)
		goto done;

	err = update_fileindex_after_commit(&commitable_paths, *new_commit_id,
	    fileindex, have_staged_files);
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	if (fileindex)
		got_fileindex_free(fileindex);
	free(fileindex_path);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	TAILQ_FOREACH(pe, &commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		free_commitable(ct);
	}
	got_pathlist_free(&commitable_paths);
	return err;
}

const char *
got_commitable_get_path(struct got_commitable *ct)
{
	return ct->path;
}

unsigned int
got_commitable_get_status(struct got_commitable *ct)
{
	return ct->status;
}

struct check_rebase_ok_arg {
	struct got_worktree *worktree;
	struct got_repository *repo;
};

static const struct got_error *
check_rebase_ok(void *arg, struct got_fileindex_entry *ie)
{
	const struct got_error *err = NULL;
	struct check_rebase_ok_arg *a = arg;
	unsigned char status;
	struct stat sb;
	char *ondisk_path;

	/* Reject rebase of a work tree with mixed base commits. */
	if (memcmp(ie->commit_sha1, a->worktree->base_commit_id->sha1,
	    SHA1_DIGEST_LENGTH))
		return got_error(GOT_ERR_MIXED_COMMITS);

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path, ie->path)
	    == -1)
		return got_error_from_errno("asprintf");

	/* Reject rebase of a work tree with modified or staged files. */
	err = get_file_status(&status, &sb, ie, ondisk_path, -1, NULL, a->repo);
	free(ondisk_path);
	if (err)
		return err;

	if (status != GOT_STATUS_NO_CHANGE)
		return got_error(GOT_ERR_MODIFIED);
	if (get_staged_status(ie) != GOT_STATUS_NO_CHANGE)
		return got_error_path(ie->path, GOT_ERR_FILE_STAGED);

	return NULL;
}

const struct got_error *
got_worktree_rebase_prepare(struct got_reference **new_base_branch_ref,
    struct got_reference **tmp_branch, struct got_fileindex **fileindex,
    struct got_worktree *worktree, struct got_reference *branch,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *tmp_branch_name = NULL, *new_base_branch_ref_name = NULL;
	char *branch_ref_name = NULL;
	char *fileindex_path = NULL;
	struct check_rebase_ok_arg ok_arg;
	struct got_reference *wt_branch = NULL, *branch_ref = NULL;
	struct got_object_id *wt_branch_tip = NULL;

	*new_base_branch_ref = NULL;
	*tmp_branch = NULL;
	*fileindex = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	ok_arg.worktree = worktree;
	ok_arg.repo = repo;
	err = got_fileindex_for_each_entry_safe(*fileindex, check_rebase_ok,
	    &ok_arg);
	if (err)
		goto done;

	err = get_rebase_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;

	err = get_newbase_symref_name(&new_base_branch_ref_name, worktree);
	if (err)
		goto done;

	err = get_rebase_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;

	err = got_ref_open(&wt_branch, repo, worktree->head_ref_name,
	    0);
	if (err)
		goto done;

	err = got_ref_resolve(&wt_branch_tip, repo, wt_branch);
	if (err)
		goto done;
	if (got_object_id_cmp(worktree->base_commit_id, wt_branch_tip) != 0) {
		err = got_error(GOT_ERR_REBASE_OUT_OF_DATE);
		goto done;
	}

	err = got_ref_alloc_symref(new_base_branch_ref,
	    new_base_branch_ref_name, wt_branch);
	if (err)
		goto done;
	err = got_ref_write(*new_base_branch_ref, repo);
	if (err)
		goto done;

	/* TODO Lock original branch's ref while rebasing? */

	err = got_ref_alloc_symref(&branch_ref, branch_ref_name, branch);
	if (err)
		goto done;

	err = got_ref_write(branch_ref, repo);
	if (err)
		goto done;

	err = got_ref_alloc(tmp_branch, tmp_branch_name,
	    worktree->base_commit_id);
	if (err)
		goto done;
	err = got_ref_write(*tmp_branch, repo);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, *tmp_branch);
	if (err)
		goto done;
done:
	free(fileindex_path);
	free(tmp_branch_name);
	free(new_base_branch_ref_name);
	free(branch_ref_name);
	if (branch_ref)
		got_ref_close(branch_ref);
	if (wt_branch)
		got_ref_close(wt_branch);
	free(wt_branch_tip);
	if (err) {
		if (*new_base_branch_ref) {
			got_ref_close(*new_base_branch_ref);
			*new_base_branch_ref = NULL;
		}
		if (*tmp_branch) {
			got_ref_close(*tmp_branch);
			*tmp_branch = NULL;
		}
		if (*fileindex) {
			got_fileindex_free(*fileindex);
			*fileindex = NULL;
		}
		lock_worktree(worktree, LOCK_SH);
	}
	return err;
}

const struct got_error *
got_worktree_rebase_continue(struct got_object_id **commit_id,
    struct got_reference **new_base_branch, struct got_reference **tmp_branch,
    struct got_reference **branch, struct got_fileindex **fileindex,
    struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err;
	char *commit_ref_name = NULL, *new_base_branch_ref_name = NULL;
	char *tmp_branch_name = NULL, *branch_ref_name = NULL;
	struct got_reference *commit_ref = NULL, *branch_ref = NULL;
	char *fileindex_path = NULL;
	int have_staged_files = 0;

	*commit_id = NULL;
	*new_base_branch = NULL;
	*tmp_branch = NULL;
	*branch = NULL;
	*fileindex = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	err = got_fileindex_for_each_entry_safe(*fileindex, check_staged_file,
	    &have_staged_files);
	if (err && err->code != GOT_ERR_CANCELLED)
		goto done;
	if (have_staged_files) {
		err = got_error(GOT_ERR_STAGED_PATHS);
		goto done;
	}

	err = get_rebase_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;

	err = get_rebase_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;

	err = get_rebase_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		goto done;

	err = get_newbase_symref_name(&new_base_branch_ref_name, worktree);
	if (err)
		goto done;

	err = got_ref_open(&branch_ref, repo, branch_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_open(branch, repo,
	    got_ref_get_symref_target(branch_ref), 0);
	if (err)
		goto done;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_resolve(commit_id, repo, commit_ref);
	if (err)
		goto done;

	err = got_ref_open(new_base_branch, repo,
	    new_base_branch_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_open(tmp_branch, repo, tmp_branch_name, 0);
	if (err)
		goto done;
done:
	free(commit_ref_name);
	free(branch_ref_name);
	free(fileindex_path);
	if (commit_ref)
		got_ref_close(commit_ref);
	if (branch_ref)
		got_ref_close(branch_ref);
	if (err) {
		free(*commit_id);
		*commit_id = NULL;
		if (*tmp_branch) {
			got_ref_close(*tmp_branch);
			*tmp_branch = NULL;
		}
		if (*new_base_branch) {
			got_ref_close(*new_base_branch);
			*new_base_branch = NULL;
		}
		if (*branch) {
			got_ref_close(*branch);
			*branch = NULL;
		}
		if (*fileindex) {
			got_fileindex_free(*fileindex);
			*fileindex = NULL;
		}
		lock_worktree(worktree, LOCK_SH);
	}
	return err;
}

const struct got_error *
got_worktree_rebase_in_progress(int *in_progress, struct got_worktree *worktree)
{
	const struct got_error *err;
	char *tmp_branch_name = NULL;

	err = get_rebase_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		return err;

	*in_progress = (strcmp(tmp_branch_name, worktree->head_ref_name) == 0);
	free(tmp_branch_name);
	return NULL;
}

static const struct got_error *
collect_rebase_commit_msg(struct got_pathlist_head *commitable_paths,
    char **logmsg, void *arg)
{
	*logmsg = arg;
	return NULL;
}

static const struct got_error *
rebase_status(void *arg, unsigned char status, unsigned char staged_status,
    const char *path, struct got_object_id *blob_id,
    struct got_object_id *staged_blob_id, struct got_object_id *commit_id,
    int dirfd, const char *de_name)
{
	return NULL;
}

struct collect_merged_paths_arg {
	got_worktree_checkout_cb progress_cb;
	void *progress_arg;
	struct got_pathlist_head *merged_paths;
};

static const struct got_error *
collect_merged_paths(void *arg, unsigned char status, const char *path)
{
	const struct got_error *err;
	struct collect_merged_paths_arg *a = arg;
	char *p;
	struct got_pathlist_entry *new;

	err = (*a->progress_cb)(a->progress_arg, status, path);
	if (err)
		return err;

	if (status != GOT_STATUS_MERGE &&
	    status != GOT_STATUS_ADD &&
	    status != GOT_STATUS_DELETE &&
	    status != GOT_STATUS_CONFLICT)
		return NULL;

	p = strdup(path);
	if (p == NULL)
		return got_error_from_errno("strdup");

	err = got_pathlist_insert(&new, a->merged_paths, p, NULL);
	if (err || new == NULL)
		free(p);
	return err;
}

void
got_worktree_rebase_pathlist_free(struct got_pathlist_head *merged_paths)
{
	struct got_pathlist_entry *pe;

	TAILQ_FOREACH(pe, merged_paths, entry)
		free((char *)pe->path);

	got_pathlist_free(merged_paths);
}

static const struct got_error *
store_commit_id(const char *commit_ref_name, struct got_object_id *commit_id,
    int is_rebase, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_reference *commit_ref = NULL;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err) {
		if (err->code != GOT_ERR_NOT_REF)
			goto done;
		err = got_ref_alloc(&commit_ref, commit_ref_name, commit_id);
		if (err)
			goto done;
		err = got_ref_write(commit_ref, repo);
		if (err)
			goto done;
	} else if (is_rebase) {
		struct got_object_id *stored_id;
		int cmp;

		err = got_ref_resolve(&stored_id, repo, commit_ref);
		if (err)
			goto done;
		cmp = got_object_id_cmp(commit_id, stored_id);
		free(stored_id);
		if (cmp != 0) {
			err = got_error(GOT_ERR_REBASE_COMMITID);
			goto done;
		}
	}
done:
	if (commit_ref)
		got_ref_close(commit_ref);
	return err;
}

static const struct got_error *
rebase_merge_files(struct got_pathlist_head *merged_paths,
    const char *commit_ref_name, struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_object_id *parent_commit_id,
    struct got_object_id *commit_id, struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	struct got_reference *commit_ref = NULL;
	struct collect_merged_paths_arg cmp_arg;
	char *fileindex_path;

	/* Work tree is locked/unlocked during rebase preparation/teardown. */

	err = get_fileindex_path(&fileindex_path, worktree);
	if (err)
		return err;

	cmp_arg.progress_cb = progress_cb;
	cmp_arg.progress_arg = progress_arg;
	cmp_arg.merged_paths = merged_paths;
	err = merge_files(worktree, fileindex, fileindex_path,
	    parent_commit_id, commit_id, repo, collect_merged_paths,
	    &cmp_arg, cancel_cb, cancel_arg);
	if (commit_ref)
		got_ref_close(commit_ref);
	return err;
}

const struct got_error *
got_worktree_rebase_merge_files(struct got_pathlist_head *merged_paths,
    struct got_worktree *worktree, struct got_fileindex *fileindex,
    struct got_object_id *parent_commit_id, struct got_object_id *commit_id,
    struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	char *commit_ref_name;

	err = get_rebase_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = store_commit_id(commit_ref_name, commit_id, 1, repo);
	if (err)
		goto done;

	err = rebase_merge_files(merged_paths, commit_ref_name, worktree,
	    fileindex, parent_commit_id, commit_id, repo, progress_cb,
	    progress_arg, cancel_cb, cancel_arg);
done:
	free(commit_ref_name);
	return err;
}

const struct got_error *
got_worktree_histedit_merge_files(struct got_pathlist_head *merged_paths,
    struct got_worktree *worktree, struct got_fileindex *fileindex,
    struct got_object_id *parent_commit_id, struct got_object_id *commit_id,
    struct got_repository *repo,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	char *commit_ref_name;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = store_commit_id(commit_ref_name, commit_id, 0, repo);
	if (err)
		goto done;

	err = rebase_merge_files(merged_paths, commit_ref_name, worktree,
	    fileindex, parent_commit_id, commit_id, repo, progress_cb,
	    progress_arg, cancel_cb, cancel_arg);
done:
	free(commit_ref_name);
	return err;
}

static const struct got_error *
rebase_commit(struct got_object_id **new_commit_id,
    struct got_pathlist_head *merged_paths, struct got_reference *commit_ref,
    struct got_worktree *worktree, struct got_fileindex *fileindex,
    struct got_reference *tmp_branch, struct got_commit_object *orig_commit,
    const char *new_logmsg, struct got_repository *repo)
{
	const struct got_error *err, *sync_err;
	struct got_pathlist_head commitable_paths;
	struct collect_commitables_arg cc_arg;
	char *fileindex_path = NULL;
	struct got_reference *head_ref = NULL;
	struct got_object_id *head_commit_id = NULL;
	char *logmsg = NULL;

	TAILQ_INIT(&commitable_paths);
	*new_commit_id = NULL;

	/* Work tree is locked/unlocked during rebase preparation/teardown. */

	err = get_fileindex_path(&fileindex_path, worktree);
	if (err)
		return err;

	cc_arg.commitable_paths = &commitable_paths;
	cc_arg.worktree = worktree;
	cc_arg.repo = repo;
	cc_arg.have_staged_files = 0;
	/*
	 * If possible get the status of individual files directly to
	 * avoid crawling the entire work tree once per rebased commit.
	 * TODO: Ideally, merged_paths would contain a list of commitables
	 * we could use so we could skip worktree_status() entirely.
	 */
	if (merged_paths) {
		struct got_pathlist_entry *pe;
		TAILQ_FOREACH(pe, merged_paths, entry) {
			err = worktree_status(worktree, pe->path, fileindex,
			    repo, collect_commitables, &cc_arg, NULL, NULL, 0,
			    0);
			if (err)
				goto done;
		}
	} else {
		err = worktree_status(worktree, "", fileindex, repo,
		    collect_commitables, &cc_arg, NULL, NULL, 0, 0);
		if (err)
			goto done;
	}

	if (TAILQ_EMPTY(&commitable_paths)) {
		/* No-op change; commit will be elided. */
		err = got_ref_delete(commit_ref, repo);
		if (err)
			goto done;
		err = got_error(GOT_ERR_COMMIT_NO_CHANGES);
		goto done;
	}

	err = got_ref_open(&head_ref, repo, worktree->head_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_resolve(&head_commit_id, repo, head_ref);
	if (err)
		goto done;

	if (new_logmsg) {
		logmsg = strdup(new_logmsg);
		if (logmsg == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	} else {
		err = got_object_commit_get_logmsg(&logmsg, orig_commit);
		if (err)
			goto done;
	}

	/* NB: commit_worktree will call free(logmsg) */
	err = commit_worktree(new_commit_id, &commitable_paths, head_commit_id,
	    worktree, got_object_commit_get_author(orig_commit),
	    got_object_commit_get_committer(orig_commit),
	    collect_rebase_commit_msg, logmsg, rebase_status, NULL, repo);
	if (err)
		goto done;

	err = got_ref_change_ref(tmp_branch, *new_commit_id);
	if (err)
		goto done;

	err = got_ref_delete(commit_ref, repo);
	if (err)
		goto done;

	err = update_fileindex_after_commit(&commitable_paths, *new_commit_id,
	    fileindex, 0);
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	free(head_commit_id);
	if (head_ref)
		got_ref_close(head_ref);
	if (err) {
		free(*new_commit_id);
		*new_commit_id = NULL;
	}
	return err;
}

const struct got_error *
got_worktree_rebase_commit(struct got_object_id **new_commit_id,
    struct got_pathlist_head *merged_paths, struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_reference *tmp_branch,
    struct got_commit_object *orig_commit,
    struct got_object_id *orig_commit_id, struct got_repository *repo)
{
	const struct got_error *err;
	char *commit_ref_name;
	struct got_reference *commit_ref = NULL;
	struct got_object_id *commit_id = NULL;

	err = get_rebase_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err)
		goto done;
	err = got_ref_resolve(&commit_id, repo, commit_ref);
	if (err)
		goto done;
	if (got_object_id_cmp(commit_id, orig_commit_id) != 0) {
		err = got_error(GOT_ERR_REBASE_COMMITID);
		goto done;
	}

	err = rebase_commit(new_commit_id, merged_paths, commit_ref,
	    worktree, fileindex, tmp_branch, orig_commit, NULL, repo);
done:
	if (commit_ref)
		got_ref_close(commit_ref);
	free(commit_ref_name);
	free(commit_id);
	return err;
}

const struct got_error *
got_worktree_histedit_commit(struct got_object_id **new_commit_id,
    struct got_pathlist_head *merged_paths, struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_reference *tmp_branch,
    struct got_commit_object *orig_commit,
    struct got_object_id *orig_commit_id, const char *new_logmsg,
    struct got_repository *repo)
{
	const struct got_error *err;
	char *commit_ref_name;
	struct got_reference *commit_ref = NULL;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err)
		goto done;

	err = rebase_commit(new_commit_id, merged_paths, commit_ref,
	    worktree, fileindex, tmp_branch, orig_commit, new_logmsg, repo);
done:
	if (commit_ref)
		got_ref_close(commit_ref);
	free(commit_ref_name);
	return err;
}

const struct got_error *
got_worktree_rebase_postpone(struct got_worktree *worktree,
    struct got_fileindex *fileindex)
{
	if (fileindex)
		got_fileindex_free(fileindex);
	return lock_worktree(worktree, LOCK_SH);
}

static const struct got_error *
delete_ref(const char *name, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_reference *ref;

	err = got_ref_open(&ref, repo, name, 0);
	if (err) {
		if (err->code == GOT_ERR_NOT_REF)
			return NULL;
		return err;
	}

	err = got_ref_delete(ref, repo);
	got_ref_close(ref);
	return err;
}

static const struct got_error *
delete_rebase_refs(struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err;
	char *tmp_branch_name = NULL, *new_base_branch_ref_name = NULL;
	char *branch_ref_name = NULL, *commit_ref_name = NULL;

	err = get_rebase_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;
	err = delete_ref(tmp_branch_name, repo);
	if (err)
		goto done;

	err = get_newbase_symref_name(&new_base_branch_ref_name, worktree);
	if (err)
		goto done;
	err = delete_ref(new_base_branch_ref_name, repo);
	if (err)
		goto done;

	err = get_rebase_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;
	err = delete_ref(branch_ref_name, repo);
	if (err)
		goto done;

	err = get_rebase_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		goto done;
	err = delete_ref(commit_ref_name, repo);
	if (err)
		goto done;

done:
	free(tmp_branch_name);
	free(new_base_branch_ref_name);
	free(branch_ref_name);
	free(commit_ref_name);
	return err;
}

const struct got_error *
got_worktree_rebase_complete(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_reference *new_base_branch,
    struct got_reference *tmp_branch, struct got_reference *rebased_branch,
    struct got_repository *repo)
{
	const struct got_error *err, *unlockerr;
	struct got_object_id *new_head_commit_id = NULL;

	err = got_ref_resolve(&new_head_commit_id, repo, tmp_branch);
	if (err)
		return err;

	err = got_ref_change_ref(rebased_branch, new_head_commit_id);
	if (err)
		goto done;

	err = got_ref_write(rebased_branch, repo);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, rebased_branch);
	if (err)
		goto done;

	err = delete_rebase_refs(worktree, repo);
done:
	if (fileindex)
		got_fileindex_free(fileindex);
	free(new_head_commit_id);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

const struct got_error *
got_worktree_rebase_abort(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_repository *repo,
    struct got_reference *new_base_branch,
     got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err, *unlockerr, *sync_err;
	struct got_reference *resolved = NULL;
	struct got_object_id *commit_id = NULL;
	char *fileindex_path = NULL;
	struct revert_file_args rfa;
	struct got_object_id *tree_id = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = got_ref_open(&resolved, repo,
	    got_ref_get_symref_target(new_base_branch), 0);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, resolved);
	if (err)
		goto done;

	/*
	 * XXX commits to the base branch could have happened while
	 * we were busy rebasing; should we store the original commit ID
	 * when rebase begins and read it back here?
	 */
	err = got_ref_resolve(&commit_id, repo, resolved);
	if (err)
		goto done;

	err = got_worktree_set_base_commit_id(worktree, repo, commit_id);
	if (err)
		goto done;

	err = got_object_id_by_path(&tree_id, repo,
	    worktree->base_commit_id, worktree->path_prefix);
	if (err)
		goto done;

	err = delete_rebase_refs(worktree, repo);
	if (err)
		goto done;

	err = get_fileindex_path(&fileindex_path, worktree);
	if (err)
		goto done;

	rfa.worktree = worktree;
	rfa.fileindex = fileindex;
	rfa.progress_cb = progress_cb;
	rfa.progress_arg = progress_arg;
	rfa.patch_cb = NULL;
	rfa.patch_arg = NULL;
	rfa.repo = repo;
	err = worktree_status(worktree, "", fileindex, repo,
	    revert_file, &rfa, NULL, NULL, 0, 0);
	if (err)
		goto sync;

	err = checkout_files(worktree, fileindex, "", tree_id, NULL,
	    repo, progress_cb, progress_arg, NULL, NULL);
sync:
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	got_ref_close(resolved);
	free(tree_id);
	free(commit_id);
	if (fileindex)
		got_fileindex_free(fileindex);
	free(fileindex_path);

	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

const struct got_error *
got_worktree_histedit_prepare(struct got_reference **tmp_branch,
    struct got_reference **branch_ref, struct got_object_id **base_commit_id,
    struct got_fileindex **fileindex, struct got_worktree *worktree,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *tmp_branch_name = NULL;
	char *branch_ref_name = NULL;
	char *base_commit_ref_name = NULL;
	char *fileindex_path = NULL;
	struct check_rebase_ok_arg ok_arg;
	struct got_reference *wt_branch = NULL;
	struct got_reference *base_commit_ref = NULL;

	*tmp_branch = NULL;
	*branch_ref = NULL;
	*base_commit_id = NULL;
	*fileindex = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	ok_arg.worktree = worktree;
	ok_arg.repo = repo;
	err = got_fileindex_for_each_entry_safe(*fileindex, check_rebase_ok,
	    &ok_arg);
	if (err)
		goto done;

	err = get_histedit_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;

	err = get_histedit_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;

	err = get_histedit_base_commit_ref_name(&base_commit_ref_name,
	    worktree);
	if (err)
		goto done;

	err = got_ref_open(&wt_branch, repo, worktree->head_ref_name,
	    0);
	if (err)
		goto done;

	err = got_ref_alloc_symref(branch_ref, branch_ref_name, wt_branch);
	if (err)
		goto done;

	err = got_ref_write(*branch_ref, repo);
	if (err)
		goto done;

	err = got_ref_alloc(&base_commit_ref, base_commit_ref_name,
	    worktree->base_commit_id);
	if (err)
		goto done;
	err = got_ref_write(base_commit_ref, repo);
	if (err)
		goto done;
	*base_commit_id = got_object_id_dup(worktree->base_commit_id);
	if (*base_commit_id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}

	err = got_ref_alloc(tmp_branch, tmp_branch_name,
	    worktree->base_commit_id);
	if (err)
		goto done;
	err = got_ref_write(*tmp_branch, repo);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, *tmp_branch);
	if (err)
		goto done;
done:
	free(fileindex_path);
	free(tmp_branch_name);
	free(branch_ref_name);
	free(base_commit_ref_name);
	if (wt_branch)
		got_ref_close(wt_branch);
	if (err) {
		if (*branch_ref) {
			got_ref_close(*branch_ref);
			*branch_ref = NULL;
		}
		if (*tmp_branch) {
			got_ref_close(*tmp_branch);
			*tmp_branch = NULL;
		}
		free(*base_commit_id);
		if (*fileindex) {
			got_fileindex_free(*fileindex);
			*fileindex = NULL;
		}
		lock_worktree(worktree, LOCK_SH);
	}
	return err;
}

const struct got_error *
got_worktree_histedit_postpone(struct got_worktree *worktree,
    struct got_fileindex *fileindex)
{
	if (fileindex)
		got_fileindex_free(fileindex);
	return lock_worktree(worktree, LOCK_SH);
}

const struct got_error *
got_worktree_histedit_in_progress(int *in_progress,
    struct got_worktree *worktree)
{
	const struct got_error *err;
	char *tmp_branch_name = NULL;

	err = get_histedit_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		return err;

	*in_progress = (strcmp(tmp_branch_name, worktree->head_ref_name) == 0);
	free(tmp_branch_name);
	return NULL;
}

const struct got_error *
got_worktree_histedit_continue(struct got_object_id **commit_id,
    struct got_reference **tmp_branch, struct got_reference **branch_ref,
    struct got_object_id **base_commit_id, struct got_fileindex **fileindex,
    struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err;
	char *commit_ref_name = NULL, *base_commit_ref_name = NULL;
	char *tmp_branch_name = NULL, *branch_ref_name = NULL;
	struct got_reference *commit_ref = NULL;
	struct got_reference *base_commit_ref = NULL;
	char *fileindex_path = NULL;
	int have_staged_files = 0;

	*commit_id = NULL;
	*tmp_branch = NULL;
	*base_commit_id = NULL;
	*fileindex = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	err = got_fileindex_for_each_entry_safe(*fileindex, check_staged_file,
	    &have_staged_files);
	if (err && err->code != GOT_ERR_CANCELLED)
		goto done;
	if (have_staged_files) {
		err = got_error(GOT_ERR_STAGED_PATHS);
		goto done;
	}

	err = get_histedit_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;

	err = get_histedit_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		goto done;

	err = get_histedit_base_commit_ref_name(&base_commit_ref_name,
	    worktree);
	if (err)
		goto done;

	err = got_ref_open(branch_ref, repo, branch_ref_name, 0);
	if (err)
		goto done;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err)
		goto done;
	err = got_ref_resolve(commit_id, repo, commit_ref);
	if (err)
		goto done;

	err = got_ref_open(&base_commit_ref, repo, base_commit_ref_name, 0);
	if (err)
		goto done;
	err = got_ref_resolve(base_commit_id, repo, base_commit_ref);
	if (err)
		goto done;

	err = got_ref_open(tmp_branch, repo, tmp_branch_name, 0);
	if (err)
		goto done;
done:
	free(commit_ref_name);
	free(branch_ref_name);
	free(fileindex_path);
	if (commit_ref)
		got_ref_close(commit_ref);
	if (base_commit_ref)
		got_ref_close(base_commit_ref);
	if (err) {
		free(*commit_id);
		*commit_id = NULL;
		free(*base_commit_id);
		*base_commit_id = NULL;
		if (*tmp_branch) {
			got_ref_close(*tmp_branch);
			*tmp_branch = NULL;
		}
		if (*fileindex) {
			got_fileindex_free(*fileindex);
			*fileindex = NULL;
		}
		lock_worktree(worktree, LOCK_EX);
	}
	return err;
}

static const struct got_error *
delete_histedit_refs(struct got_worktree *worktree, struct got_repository *repo)
{
	const struct got_error *err;
	char *tmp_branch_name = NULL, *base_commit_ref_name = NULL;
	char *branch_ref_name = NULL, *commit_ref_name = NULL;

	err = get_histedit_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		goto done;
	err = delete_ref(tmp_branch_name, repo);
	if (err)
		goto done;

	err = get_histedit_base_commit_ref_name(&base_commit_ref_name,
	    worktree);
	if (err)
		goto done;
	err = delete_ref(base_commit_ref_name, repo);
	if (err)
		goto done;

	err = get_histedit_branch_symref_name(&branch_ref_name, worktree);
	if (err)
		goto done;
	err = delete_ref(branch_ref_name, repo);
	if (err)
		goto done;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		goto done;
	err = delete_ref(commit_ref_name, repo);
	if (err)
		goto done;
done:
	free(tmp_branch_name);
	free(base_commit_ref_name);
	free(branch_ref_name);
	free(commit_ref_name);
	return err;
}

const struct got_error *
got_worktree_histedit_abort(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_repository *repo,
    struct got_reference *branch, struct got_object_id *base_commit_id,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err, *unlockerr, *sync_err;
	struct got_reference *resolved = NULL;
	char *fileindex_path = NULL;
	struct got_object_id *tree_id = NULL;
	struct revert_file_args rfa;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = got_ref_open(&resolved, repo,
	    got_ref_get_symref_target(branch), 0);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, resolved);
	if (err)
		goto done;

	err = got_worktree_set_base_commit_id(worktree, repo, base_commit_id);
	if (err)
		goto done;

	err = got_object_id_by_path(&tree_id, repo, base_commit_id,
	    worktree->path_prefix);
	if (err)
		goto done;

	err = delete_histedit_refs(worktree, repo);
	if (err)
		goto done;

	err = get_fileindex_path(&fileindex_path, worktree);
	if (err)
		goto done;

	rfa.worktree = worktree;
	rfa.fileindex = fileindex;
	rfa.progress_cb = progress_cb;
	rfa.progress_arg = progress_arg;
	rfa.patch_cb = NULL;
	rfa.patch_arg = NULL;
	rfa.repo = repo;
	err = worktree_status(worktree, "", fileindex, repo,
	    revert_file, &rfa, NULL, NULL, 0, 0);
	if (err)
		goto sync;

	err = checkout_files(worktree, fileindex, "", tree_id, NULL,
	    repo, progress_cb, progress_arg, NULL, NULL);
sync:
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	got_ref_close(resolved);
	free(tree_id);
	free(fileindex_path);

	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

const struct got_error *
got_worktree_histedit_complete(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_reference *tmp_branch,
    struct got_reference *edited_branch, struct got_repository *repo)
{
	const struct got_error *err, *unlockerr;
	struct got_object_id *new_head_commit_id = NULL;
	struct got_reference *resolved = NULL;

	err = got_ref_resolve(&new_head_commit_id, repo, tmp_branch);
	if (err)
		return err;

	err = got_ref_open(&resolved, repo,
	    got_ref_get_symref_target(edited_branch), 0);
	if (err)
		goto done;

	err = got_ref_change_ref(resolved, new_head_commit_id);
	if (err)
		goto done;

	err = got_ref_write(resolved, repo);
	if (err)
		goto done;

	err = got_worktree_set_head_ref(worktree, resolved);
	if (err)
		goto done;

	err = delete_histedit_refs(worktree, repo);
done:
	if (fileindex)
		got_fileindex_free(fileindex);
	free(new_head_commit_id);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

const struct got_error *
got_worktree_histedit_skip_commit(struct got_worktree *worktree,
    struct got_object_id *commit_id, struct got_repository *repo)
{
	const struct got_error *err;
	char *commit_ref_name;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = store_commit_id(commit_ref_name, commit_id, 0, repo);
	if (err)
		goto done;

	err = delete_ref(commit_ref_name, repo);
done:
	free(commit_ref_name);
	return err;
}

const struct got_error *
got_worktree_integrate_prepare(struct got_fileindex **fileindex,
    struct got_reference **branch_ref, struct got_reference **base_branch_ref,
    struct got_worktree *worktree, const char *refname,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *fileindex_path = NULL;
	struct check_rebase_ok_arg ok_arg;

	*fileindex = NULL;
	*branch_ref = NULL;
	*base_branch_ref = NULL;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	if (strcmp(refname, got_worktree_get_head_ref_name(worktree)) == 0) {
		err = got_error_msg(GOT_ERR_SAME_BRANCH,
		    "cannot integrate a branch into itself; "
		    "update -b or different branch name required");
		goto done;
	}

	err = open_fileindex(fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	/* Preconditions are the same as for rebase. */
	ok_arg.worktree = worktree;
	ok_arg.repo = repo;
	err = got_fileindex_for_each_entry_safe(*fileindex, check_rebase_ok,
	    &ok_arg);
	if (err)
		goto done;

	err = got_ref_open(branch_ref, repo, refname, 1);
	if (err)
		goto done;

	err = got_ref_open(base_branch_ref, repo,
	    got_worktree_get_head_ref_name(worktree), 1);
done:
	if (err) {
		if (*branch_ref) {
			got_ref_close(*branch_ref);
			*branch_ref = NULL;
		}
		if (*base_branch_ref) {
			got_ref_close(*base_branch_ref);
			*base_branch_ref = NULL;
		}
		if (*fileindex) {
			got_fileindex_free(*fileindex);
			*fileindex = NULL;
		}
		lock_worktree(worktree, LOCK_SH);
	}
	return err;
}

const struct got_error *
got_worktree_integrate_continue(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_repository *repo,
    struct got_reference *branch_ref, struct got_reference *base_branch_ref,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	char *fileindex_path = NULL;
	struct got_object_id *tree_id = NULL, *commit_id = NULL;

	err = get_fileindex_path(&fileindex_path, worktree);
	if (err)
		goto done;

	err = got_ref_resolve(&commit_id, repo, branch_ref);
	if (err)
		goto done;

	err = got_object_id_by_path(&tree_id, repo, commit_id,
	    worktree->path_prefix);
	if (err)
		goto done;

	err = got_worktree_set_base_commit_id(worktree, repo, commit_id);
	if (err)
		goto done;

	err = checkout_files(worktree, fileindex, "", tree_id, NULL, repo,
	    progress_cb, progress_arg, cancel_cb, cancel_arg);
	if (err)
		goto sync;

	err = got_ref_change_ref(base_branch_ref, commit_id);
	if (err)
		goto sync;

	err = got_ref_write(base_branch_ref, repo);
sync:
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;

done:
	unlockerr = got_ref_unlock(branch_ref);
	if (unlockerr && err == NULL)
		err = unlockerr;
	got_ref_close(branch_ref);

	unlockerr = got_ref_unlock(base_branch_ref);
	if (unlockerr && err == NULL)
		err = unlockerr;
	got_ref_close(base_branch_ref);

	got_fileindex_free(fileindex);
	free(fileindex_path);
	free(tree_id);

	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

const struct got_error *
got_worktree_integrate_abort(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_repository *repo,
    struct got_reference *branch_ref, struct got_reference *base_branch_ref)
{
	const struct got_error *err = NULL, *unlockerr = NULL;

	got_fileindex_free(fileindex);

	err = lock_worktree(worktree, LOCK_SH);

	unlockerr = got_ref_unlock(branch_ref);
	if (unlockerr && err == NULL)
		err = unlockerr;
	got_ref_close(branch_ref);

	unlockerr = got_ref_unlock(base_branch_ref);
	if (unlockerr && err == NULL)
		err = unlockerr;
	got_ref_close(base_branch_ref);

	return err;
}

struct check_stage_ok_arg {
	struct got_object_id *head_commit_id;
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	struct got_repository *repo;
	int have_changes;
};

const struct got_error *
check_stage_ok(void *arg, unsigned char status,
    unsigned char staged_status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *staged_blob_id,
    struct got_object_id *commit_id, int dirfd, const char *de_name)
{
	struct check_stage_ok_arg *a = arg;
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie;
	struct got_object_id base_commit_id;
	struct got_object_id *base_commit_idp = NULL;
	char *in_repo_path = NULL, *p;

	if (status == GOT_STATUS_UNVERSIONED ||
	    status == GOT_STATUS_NO_CHANGE)
		return NULL;
	if (status == GOT_STATUS_NONEXISTENT)
		return got_error_set_errno(ENOENT, relpath);

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie == NULL)
		return got_error_path(relpath, GOT_ERR_FILE_STATUS);

	if (asprintf(&in_repo_path, "%s%s%s", a->worktree->path_prefix,
	    got_path_is_root_dir(a->worktree->path_prefix) ? "" : "/",
	    relpath) == -1)
		return got_error_from_errno("asprintf");

	if (got_fileindex_entry_has_commit(ie)) {
		memcpy(base_commit_id.sha1, ie->commit_sha1,
		    SHA1_DIGEST_LENGTH);
		base_commit_idp = &base_commit_id;
	}

	if (status == GOT_STATUS_CONFLICT) {
		err = got_error_path(ie->path, GOT_ERR_STAGE_CONFLICT);
		goto done;
	} else if (status != GOT_STATUS_ADD &&
	    status != GOT_STATUS_MODIFY &&
	    status != GOT_STATUS_DELETE) {
		err = got_error_path(ie->path, GOT_ERR_FILE_STATUS);
		goto done;
	}

	a->have_changes = 1;

	p = in_repo_path;
	while (p[0] == '/')
		p++;
	err = check_out_of_date(p, status, staged_status,
	    blob_id, base_commit_idp, a->head_commit_id, a->repo,
	    GOT_ERR_STAGE_OUT_OF_DATE);
done:
	free(in_repo_path);
	return err;
}

struct stage_path_arg {
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	struct got_repository *repo;
	got_worktree_status_cb status_cb;
	void *status_arg;
	got_worktree_patch_cb patch_cb;
	void *patch_arg;
	int staged_something;
	int allow_bad_symlinks;
};

static const struct got_error *
stage_path(void *arg, unsigned char status,
    unsigned char staged_status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *staged_blob_id,
    struct got_object_id *commit_id, int dirfd, const char *de_name)
{
	struct stage_path_arg *a = arg;
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie;
	char *ondisk_path = NULL, *path_content = NULL;
	uint32_t stage;
	struct got_object_id *new_staged_blob_id = NULL;
	struct stat sb;

	if (status == GOT_STATUS_UNVERSIONED)
		return NULL;

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie == NULL)
		return got_error_path(relpath, GOT_ERR_FILE_STATUS);

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
	    relpath)== -1)
		return got_error_from_errno("asprintf");

	switch (status) {
	case GOT_STATUS_ADD:
	case GOT_STATUS_MODIFY:
		/* XXX could sb.st_mode be passed in by our caller? */
		if (lstat(ondisk_path, &sb) == -1) {
			err = got_error_from_errno2("lstat", ondisk_path);
			break;
		}
		if (a->patch_cb) {
			if (status == GOT_STATUS_ADD) {
				int choice = GOT_PATCH_CHOICE_NONE;
				err = (*a->patch_cb)(&choice, a->patch_arg,
				    status, ie->path, NULL, 1, 1);
				if (err)
					break;
				if (choice != GOT_PATCH_CHOICE_YES)
					break;
			} else {
				err = create_patched_content(&path_content, 0,
				    staged_blob_id ? staged_blob_id : blob_id,
				    ondisk_path, dirfd, de_name, ie->path,
				    a->repo, a->patch_cb, a->patch_arg);
				if (err || path_content == NULL)
					break;
			}
		}
		err = got_object_blob_create(&new_staged_blob_id,
		    path_content ? path_content : ondisk_path, a->repo);
		if (err)
			break;
		memcpy(ie->staged_blob_sha1, new_staged_blob_id->sha1,
		    SHA1_DIGEST_LENGTH);
		if (status == GOT_STATUS_ADD || staged_status == GOT_STATUS_ADD)
			stage = GOT_FILEIDX_STAGE_ADD;
		else
			stage = GOT_FILEIDX_STAGE_MODIFY;
		got_fileindex_entry_stage_set(ie, stage);
		if (S_ISLNK(sb.st_mode)) {
			int is_bad_symlink = 0;
			if (!a->allow_bad_symlinks) {
				char target_path[PATH_MAX];
				ssize_t target_len;
				target_len = readlink(ondisk_path, target_path,
				    sizeof(target_path));
				if (target_len == -1) {
					err = got_error_from_errno2("readlink",
					    ondisk_path);
					break;
				}
				err = is_bad_symlink_target(&is_bad_symlink,
				    target_path, target_len, ondisk_path,
				    a->worktree->root_path);
				if (err)
					break;
				if (is_bad_symlink) {
					err = got_error_path(ondisk_path,
					    GOT_ERR_BAD_SYMLINK);
					break;
				}
			}
			if (is_bad_symlink)
				got_fileindex_entry_staged_filetype_set(ie,
				    GOT_FILEIDX_MODE_BAD_SYMLINK);
			else
				got_fileindex_entry_staged_filetype_set(ie,
				    GOT_FILEIDX_MODE_SYMLINK);
		} else {
			got_fileindex_entry_staged_filetype_set(ie,
			    GOT_FILEIDX_MODE_REGULAR_FILE);
		}
		a->staged_something = 1;
		if (a->status_cb == NULL)
			break;
		err = (*a->status_cb)(a->status_arg, GOT_STATUS_NO_CHANGE,
		    get_staged_status(ie), relpath, blob_id,
		    new_staged_blob_id, NULL, dirfd, de_name);
		break;
	case GOT_STATUS_DELETE:
		if (staged_status == GOT_STATUS_DELETE)
			break;
		if (a->patch_cb) {
			int choice = GOT_PATCH_CHOICE_NONE;
			err = (*a->patch_cb)(&choice, a->patch_arg, status,
			    ie->path, NULL, 1, 1);
			if (err)
				break;
			if (choice == GOT_PATCH_CHOICE_NO)
				break;
			if (choice != GOT_PATCH_CHOICE_YES) {
				err = got_error(GOT_ERR_PATCH_CHOICE);
				break;
			}
		}
		stage = GOT_FILEIDX_STAGE_DELETE;
		got_fileindex_entry_stage_set(ie, stage);
		a->staged_something = 1;
		if (a->status_cb == NULL)
			break;
		err = (*a->status_cb)(a->status_arg, GOT_STATUS_NO_CHANGE,
		    get_staged_status(ie), relpath, NULL, NULL, NULL, dirfd,
		    de_name);
		break;
	case GOT_STATUS_NO_CHANGE:
		break;
	case GOT_STATUS_CONFLICT:
		err = got_error_path(relpath, GOT_ERR_STAGE_CONFLICT);
		break;
	case GOT_STATUS_NONEXISTENT:
		err = got_error_set_errno(ENOENT, relpath);
		break;
	default:
		err = got_error_path(relpath, GOT_ERR_FILE_STATUS);
		break;
	}

	if (path_content && unlink(path_content) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path_content);
	free(path_content);
	free(ondisk_path);
	free(new_staged_blob_id);
	return err;
}

const struct got_error *
got_worktree_stage(struct got_worktree *worktree,
    struct got_pathlist_head *paths,
    got_worktree_status_cb status_cb, void *status_arg,
    got_worktree_patch_cb patch_cb, void *patch_arg,
    int allow_bad_symlinks, struct got_repository *repo)
{
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	struct got_reference *head_ref = NULL;
	struct got_object_id *head_commit_id = NULL;
	struct check_stage_ok_arg oka;
	struct stage_path_arg spa;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = got_ref_open(&head_ref, repo,
	    got_worktree_get_head_ref_name(worktree), 0);
	if (err)
		goto done;
	err = got_ref_resolve(&head_commit_id, repo, head_ref);
	if (err)
		goto done;
	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	/* Check pre-conditions before staging anything. */
	oka.head_commit_id = head_commit_id;
	oka.worktree = worktree;
	oka.fileindex = fileindex;
	oka.repo = repo;
	oka.have_changes = 0;
	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
		    check_stage_ok, &oka, NULL, NULL, 0, 0);
		if (err)
			goto done;
	}
	if (!oka.have_changes) {
		err = got_error(GOT_ERR_STAGE_NO_CHANGE);
		goto done;
	}

	spa.worktree = worktree;
	spa.fileindex = fileindex;
	spa.repo = repo;
	spa.patch_cb = patch_cb;
	spa.patch_arg = patch_arg;
	spa.status_cb = status_cb;
	spa.status_arg = status_arg;
	spa.staged_something = 0;
	spa.allow_bad_symlinks = allow_bad_symlinks;
	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
		    stage_path, &spa, NULL, NULL, 0, 0);
		if (err)
			goto done;
	}
	if (!spa.staged_something) {
		err = got_error(GOT_ERR_STAGE_NO_CHANGE);
		goto done;
	}

	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	if (head_ref)
		got_ref_close(head_ref);
	free(head_commit_id);
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

struct unstage_path_arg {
	struct got_worktree *worktree;
	struct got_fileindex *fileindex;
	struct got_repository *repo;
	got_worktree_checkout_cb progress_cb;
	void *progress_arg;
	got_worktree_patch_cb patch_cb;
	void *patch_arg;
};

static const struct got_error *
create_unstaged_content(char **path_unstaged_content,
    char **path_new_staged_content, struct got_object_id *blob_id,
    struct got_object_id *staged_blob_id, const char *relpath,
    struct got_repository *repo,
    got_worktree_patch_cb patch_cb, void *patch_arg)
{
	const struct got_error *err;
	struct got_blob_object *blob = NULL, *staged_blob = NULL;
	FILE *f1 = NULL, *f2 = NULL, *outfile = NULL, *rejectfile = NULL;
	char *path1 = NULL, *path2 = NULL, *label1 = NULL;
	struct stat sb1, sb2;
	struct got_diff_changes *changes = NULL;
	struct got_diff_state *ds = NULL;
	struct got_diff_args *args = NULL;
	struct got_diff_change *change;
	int diff_flags = 0, line_cur1 = 1, line_cur2 = 1, n = 0;
	int have_content = 0, have_rejected_content = 0;

	*path_unstaged_content = NULL;
	*path_new_staged_content = NULL;

	err = got_object_id_str(&label1, blob_id);
	if (err)
		return err;
	err = got_object_open_as_blob(&blob, repo, blob_id, 8192);
	if (err)
		goto done;

	err = got_opentemp_named(&path1, &f1, "got-unstage-blob-base");
	if (err)
		goto done;

	err = got_object_blob_dump_to_file(NULL, NULL, NULL, f1, blob);
	if (err)
		goto done;

	err = got_object_open_as_blob(&staged_blob, repo, staged_blob_id, 8192);
	if (err)
		goto done;

	err = got_opentemp_named(&path2, &f2, "got-unstage-blob-staged");
	if (err)
		goto done;

	err = got_object_blob_dump_to_file(NULL, NULL, NULL, f2, staged_blob);
	if (err)
		goto done;

	if (stat(path1, &sb1) == -1) {
		err = got_error_from_errno2("stat", path1);
		goto done;
	}

	if (stat(path2, &sb2) == -1) {
		err = got_error_from_errno2("stat", path2);
		goto done;
	}

	err = got_diff_files(&changes, &ds, &args, &diff_flags,
	    f1, sb1.st_size, label1, f2, sb2.st_size, path2, 3, NULL);
	if (err)
		goto done;

	err = got_opentemp_named(path_unstaged_content, &outfile,
	    "got-unstaged-content");
	if (err)
		goto done;
	err = got_opentemp_named(path_new_staged_content, &rejectfile,
	    "got-new-staged-content");
	if (err)
		goto done;

	if (fseek(f1, 0L, SEEK_SET) == -1) {
		err = got_ferror(f1, GOT_ERR_IO);
		goto done;
	}
	if (fseek(f2, 0L, SEEK_SET) == -1) {
		err = got_ferror(f2, GOT_ERR_IO);
		goto done;
	}
	SIMPLEQ_FOREACH(change, &changes->entries, entry) {
		int choice;
		err = apply_or_reject_change(&choice, change, ++n,
		    changes->nchanges, ds, args, diff_flags, relpath,
		    f1, f2, &line_cur1, &line_cur2,
		    outfile, rejectfile, patch_cb, patch_arg);
		if (err)
			goto done;
		if (choice == GOT_PATCH_CHOICE_YES)
			have_content = 1;
		else
			have_rejected_content = 1;
		if (choice == GOT_PATCH_CHOICE_QUIT)
			break;
	}
	if (have_content || have_rejected_content)
		err = copy_remaining_content(f1, f2, &line_cur1, &line_cur2,
		    outfile, rejectfile);
done:
	free(label1);
	if (blob)
		got_object_blob_close(blob);
	if (staged_blob)
		got_object_blob_close(staged_blob);
	if (f1 && fclose(f1) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path1);
	if (f2 && fclose(f2) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", path2);
	if (outfile && fclose(outfile) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", *path_unstaged_content);
	if (rejectfile && fclose(rejectfile) == EOF && err == NULL)
		err = got_error_from_errno2("fclose", *path_new_staged_content);
	if (path1 && unlink(path1) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path1);
	if (path2 && unlink(path2) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path2);
	if (err || !have_content) {
		if (*path_unstaged_content &&
		    unlink(*path_unstaged_content) == -1 && err == NULL)
			err = got_error_from_errno2("unlink",
			    *path_unstaged_content);
		free(*path_unstaged_content);
		*path_unstaged_content = NULL;
	}
	if (err || !have_content || !have_rejected_content) {
		if (*path_new_staged_content &&
		    unlink(*path_new_staged_content) == -1 && err == NULL)
			err = got_error_from_errno2("unlink",
			    *path_new_staged_content);
		free(*path_new_staged_content);
		*path_new_staged_content = NULL;
	}
	free(args);
	if (ds) {
		got_diff_state_free(ds);
		free(ds);
	}
	if (changes)
		got_diff_free_changes(changes);
	free(path1);
	free(path2);
	return err;
}

static const struct got_error *
unstage_hunks(struct got_object_id *staged_blob_id,
    struct got_blob_object *blob_base,
    struct got_object_id *blob_id, struct got_fileindex_entry *ie,
    const char *ondisk_path, const char *label_orig,
    struct got_worktree *worktree, struct got_repository *repo,
    got_worktree_patch_cb patch_cb, void *patch_arg,
    got_worktree_checkout_cb progress_cb, void *progress_arg)
{
	const struct got_error *err = NULL;
	char *path_unstaged_content = NULL;
	char *path_new_staged_content = NULL;
	struct got_object_id *new_staged_blob_id = NULL;
	FILE *f = NULL;
	struct stat sb;

	err = create_unstaged_content(&path_unstaged_content,
	    &path_new_staged_content, blob_id, staged_blob_id,
	    ie->path, repo, patch_cb, patch_arg);
	if (err)
		return err;

	if (path_unstaged_content == NULL)
		return NULL;

	if (path_new_staged_content) {
		err = got_object_blob_create(&new_staged_blob_id,
		    path_new_staged_content, repo);
		if (err)
			goto done;
	}

	f = fopen(path_unstaged_content, "r");
	if (f == NULL) {
		err = got_error_from_errno2("fopen",
		    path_unstaged_content);
		goto done;
	}
	if (fstat(fileno(f), &sb) == -1) {
		err = got_error_from_errno2("fstat", path_unstaged_content);
		goto done;
	}
	if (got_fileindex_entry_staged_filetype_get(ie) ==
	    GOT_FILEIDX_MODE_SYMLINK && sb.st_size < PATH_MAX) {
		char link_target[PATH_MAX];
		size_t r;
		r = fread(link_target, 1, sizeof(link_target), f);
		if (r == 0 && ferror(f)) {
			err = got_error_from_errno("fread");
			goto done;
		}
		if (r >= sizeof(link_target)) { /* should not happen */
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
		link_target[r] = '\0';
		err = merge_symlink(worktree, blob_base,
		    ondisk_path, ie->path, label_orig, link_target,
		    worktree->base_commit_id, repo, progress_cb,
		    progress_arg);
	} else {
		int local_changes_subsumed;
		err = merge_file(&local_changes_subsumed, worktree,
		    blob_base, ondisk_path, ie->path,
		    got_fileindex_perms_to_st(ie),
		    path_unstaged_content, label_orig, "unstaged",
		    repo, progress_cb, progress_arg);
	}
	if (err)
		goto done;

	if (new_staged_blob_id) {
		memcpy(ie->staged_blob_sha1, new_staged_blob_id->sha1,
		    SHA1_DIGEST_LENGTH);
	} else
		got_fileindex_entry_stage_set(ie, GOT_FILEIDX_STAGE_NONE);
done:
	free(new_staged_blob_id);
	if (path_unstaged_content &&
	    unlink(path_unstaged_content) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path_unstaged_content);
	if (path_new_staged_content &&
	    unlink(path_new_staged_content) == -1 && err == NULL)
		err = got_error_from_errno2("unlink", path_new_staged_content);
	if (f && fclose(f) != 0 && err == NULL)
		err = got_error_from_errno2("fclose", path_unstaged_content);
	free(path_unstaged_content);
	free(path_new_staged_content);
	return err;
}

static const struct got_error *
unstage_path(void *arg, unsigned char status,
    unsigned char staged_status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *staged_blob_id,
    struct got_object_id *commit_id, int dirfd, const char *de_name)
{
	const struct got_error *err = NULL;
	struct unstage_path_arg *a = arg;
	struct got_fileindex_entry *ie;
	struct got_blob_object *blob_base = NULL, *blob_staged = NULL;
	char *ondisk_path = NULL;
	char *id_str = NULL, *label_orig = NULL;
	int local_changes_subsumed;
	struct stat sb;

	if (staged_status != GOT_STATUS_ADD &&
	    staged_status != GOT_STATUS_MODIFY &&
	    staged_status != GOT_STATUS_DELETE)
		return NULL;

	ie = got_fileindex_entry_get(a->fileindex, relpath, strlen(relpath));
	if (ie == NULL)
		return got_error_path(relpath, GOT_ERR_FILE_STATUS);

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path, relpath)
	    == -1)
		return got_error_from_errno("asprintf");

	err = got_object_id_str(&id_str,
	    commit_id ? commit_id : a->worktree->base_commit_id);
	if (err)
		goto done;
	if (asprintf(&label_orig, "%s: commit %s", GOT_MERGE_LABEL_BASE,
	    id_str) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	switch (staged_status) {
	case GOT_STATUS_MODIFY:
		err = got_object_open_as_blob(&blob_base, a->repo,
		    blob_id, 8192);
		if (err)
			break;
		/* fall through */
	case GOT_STATUS_ADD:
		if (a->patch_cb) {
			if (staged_status == GOT_STATUS_ADD) {
				int choice = GOT_PATCH_CHOICE_NONE;
				err = (*a->patch_cb)(&choice, a->patch_arg,
				    staged_status, ie->path, NULL, 1, 1);
				if (err)
					break;
				if (choice != GOT_PATCH_CHOICE_YES)
					break;
			} else {
				err = unstage_hunks(staged_blob_id,
				    blob_base, blob_id, ie, ondisk_path,
				    label_orig, a->worktree, a->repo,
				    a->patch_cb, a->patch_arg,
				    a->progress_cb, a->progress_arg);
				break; /* Done with this file. */
			}
		}
		err = got_object_open_as_blob(&blob_staged, a->repo,
		    staged_blob_id, 8192);
		if (err)
			break;
		switch (got_fileindex_entry_staged_filetype_get(ie)) {
		case GOT_FILEIDX_MODE_BAD_SYMLINK:
		case GOT_FILEIDX_MODE_REGULAR_FILE:
			err = merge_blob(&local_changes_subsumed, a->worktree,
			    blob_base, ondisk_path, relpath,
			    got_fileindex_perms_to_st(ie), label_orig,
			    blob_staged, commit_id ? commit_id :
			    a->worktree->base_commit_id, a->repo,
			    a->progress_cb, a->progress_arg);
			break;
		case GOT_FILEIDX_MODE_SYMLINK:
			if (S_ISLNK(got_fileindex_perms_to_st(ie))) {
				char *staged_target;
				err = got_object_blob_read_to_str(
				    &staged_target, blob_staged);
				if (err)
					goto done;
				err = merge_symlink(a->worktree, blob_base,
				    ondisk_path, relpath, label_orig,
				    staged_target, commit_id ? commit_id :
				    a->worktree->base_commit_id,
				    a->repo, a->progress_cb, a->progress_arg);
				free(staged_target);
			} else {
				err = merge_blob(&local_changes_subsumed,
				    a->worktree, blob_base, ondisk_path,
				    relpath, got_fileindex_perms_to_st(ie),
				    label_orig, blob_staged,
				    commit_id ? commit_id :
				    a->worktree->base_commit_id, a->repo,
				    a->progress_cb, a->progress_arg);
			}
			break;
		default:
			err = got_error_path(relpath, GOT_ERR_BAD_FILETYPE);
			break;
		}
		if (err == NULL)
			got_fileindex_entry_stage_set(ie,
			    GOT_FILEIDX_STAGE_NONE);
		break;
	case GOT_STATUS_DELETE:
		if (a->patch_cb) {
			int choice = GOT_PATCH_CHOICE_NONE;
			err = (*a->patch_cb)(&choice, a->patch_arg,
			    staged_status, ie->path, NULL, 1, 1);
			if (err)
				break;
			if (choice == GOT_PATCH_CHOICE_NO)
				break;
			if (choice != GOT_PATCH_CHOICE_YES) {
				err = got_error(GOT_ERR_PATCH_CHOICE);
				break;
			}
		}
		got_fileindex_entry_stage_set(ie, GOT_FILEIDX_STAGE_NONE);
		err = get_file_status(&status, &sb, ie, ondisk_path,
		    dirfd, de_name, a->repo);
		if (err)
			break;
		err = (*a->progress_cb)(a->progress_arg, status, relpath);
		break;
	}
done:
	free(ondisk_path);
	if (blob_base)
		got_object_blob_close(blob_base);
	if (blob_staged)
		got_object_blob_close(blob_staged);
	free(id_str);
	free(label_orig);
	return err;
}

const struct got_error *
got_worktree_unstage(struct got_worktree *worktree,
    struct got_pathlist_head *paths,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    got_worktree_patch_cb patch_cb, void *patch_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	struct unstage_path_arg upa;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	upa.worktree = worktree;
	upa.fileindex = fileindex;
	upa.repo = repo;
	upa.progress_cb = progress_cb;
	upa.progress_arg = progress_arg;
	upa.patch_cb = patch_cb;
	upa.patch_arg = patch_arg;
	TAILQ_FOREACH(pe, paths, entry) {
		err = worktree_status(worktree, pe->path, fileindex, repo,
		    unstage_path, &upa, NULL, NULL, 0, 0);
		if (err)
			goto done;
	}

	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_SH);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}

struct report_file_info_arg {
	struct got_worktree *worktree;
	got_worktree_path_info_cb info_cb;
	void *info_arg;
	struct got_pathlist_head *paths;
	got_cancel_cb cancel_cb;
	void *cancel_arg;
};

static const struct got_error *
report_file_info(void *arg, struct got_fileindex_entry *ie)
{
	struct report_file_info_arg *a = arg;
	struct got_pathlist_entry *pe;
	struct got_object_id blob_id, staged_blob_id, commit_id;
	struct got_object_id *blob_idp = NULL, *staged_blob_idp = NULL;
	struct got_object_id *commit_idp = NULL;
	int stage;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	TAILQ_FOREACH(pe, a->paths, entry) {
		if (pe->path_len == 0 || strcmp(pe->path, ie->path) == 0 ||
		    got_path_is_child(ie->path, pe->path, pe->path_len))
			break;
	}
	if (pe == NULL) /* not found */
		return NULL;

	if (got_fileindex_entry_has_blob(ie)) {
		memcpy(blob_id.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
		blob_idp = &blob_id;
	}
	stage = got_fileindex_entry_stage_get(ie);
	if (stage == GOT_FILEIDX_STAGE_MODIFY ||
	    stage == GOT_FILEIDX_STAGE_ADD) {
		memcpy(staged_blob_id.sha1, ie->staged_blob_sha1,
		    SHA1_DIGEST_LENGTH);
		staged_blob_idp = &staged_blob_id;
	}

	if (got_fileindex_entry_has_commit(ie)) {
		memcpy(commit_id.sha1, ie->commit_sha1, SHA1_DIGEST_LENGTH);
		commit_idp = &commit_id;
	}

	return a->info_cb(a->info_arg, ie->path, got_fileindex_perms_to_st(ie),
	    (time_t)ie->mtime_sec, blob_idp, staged_blob_idp, commit_idp);
}

const struct got_error *
got_worktree_path_info(struct got_worktree *worktree,
    struct got_pathlist_head *paths,
    got_worktree_path_info_cb info_cb, void *info_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)

{
	const struct got_error *err = NULL, *unlockerr;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	struct report_file_info_arg arg;

	err = lock_worktree(worktree, LOCK_SH);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	arg.worktree = worktree;
	arg.info_cb = info_cb;
	arg.info_arg = info_arg;
	arg.paths = paths;
	arg.cancel_cb = cancel_cb;
	arg.cancel_arg = cancel_arg;
	err = got_fileindex_for_each_entry_safe(fileindex, report_file_info,
	    &arg);
done:
	free(fileindex_path);
	if (fileindex)
		got_fileindex_free(fileindex);
	unlockerr = lock_worktree(worktree, LOCK_UN);
	if (unlockerr && err == NULL)
		err = unlockerr;
	return err;
}
