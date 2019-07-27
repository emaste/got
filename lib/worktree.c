/*
 * Copyright (c) 2018, 2019 Stefan Sperling <stsp@openbsd.org>
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
#include <sys/limits.h>
#include <sys/queue.h>
#include <sys/tree.h>

#include <dirent.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sha1.h>
#include <zlib.h>
#include <fnmatch.h>
#include <libgen.h>
#include <uuid.h>
#include <util.h>

#include "got_error.h"
#include "got_repository.h"
#include "got_reference.h"
#include "got_object.h"
#include "got_path.h"
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

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

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
			err = got_error(GOT_ERR_WORKTREE_META);
		else
			err = got_error_from_errno2("open", path);
		goto done;
	}
	if (flock(fd, LOCK_SH | LOCK_NB) == -1) {
		err = (errno == EWOULDBLOCK ? got_error(GOT_ERR_WORKTREE_BUSY)
		    : got_error_from_errno2("flock", path));
		goto done;
	}

	if (lstat(path, &sb) != 0) {
		err = got_error_from_errno2("lstat", path);
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
		    got_error(GOT_ERR_WORKTREE_META));
		goto done;
	}
	if ((*content)[sb.st_size - 1] != '\n') {
		err = got_error(GOT_ERR_WORKTREE_META);
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
		err = got_error_uuid(uuid_status);
		goto done;
	}
	uuid_to_string(&uuid, &uuidstr, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		err = got_error_uuid(uuid_status);
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
		err = got_error(GOT_ERR_WORKTREE_META);
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

	(*worktree)->root_path = strdup(path);
	if ((*worktree)->root_path == NULL) {
		err = got_error_from_errno("strdup");
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
		err = got_error_uuid(uuid_status);
		goto done;
	}

	err = got_repo_open(&repo, (*worktree)->repo_path);
	if (err)
		goto done;

	err = got_object_resolve_id_str(&(*worktree)->base_commit_id, repo,
	    base_commit_id_str);
	if (err)
		goto done;

	err = read_meta_file(&(*worktree)->head_ref_name, path_got,
	    GOT_WORKTREE_HEAD_REF);
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
	free(worktree->root_path);
	free(worktree->repo_path);
	free(worktree->path_prefix);
	free(worktree->base_commit_id);
	free(worktree->head_ref_name);
	if (worktree->lockfd != -1)
		if (close(worktree->lockfd) != 0)
			err = got_error_from_errno2("close",
			    got_worktree_get_root_path(worktree));
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
			err = got_error(GOT_ERR_FILE_OBSTRUCTED);
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
		return got_error_from_errno2("open", f1_path);

	f2 = fopen(f2_path, "r");
	if (f2 == NULL) {
		err = got_error_from_errno2("open", f2_path);
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
 * blob_deriv acts as the first derived version, and the file on disk
 * acts as the second derived version.
 */
static const struct got_error *
merge_blob(int *local_changes_subsumed, struct got_worktree *worktree,
    struct got_blob_object *blob_orig, const char *ondisk_path,
    const char *path, uint16_t st_mode, struct got_blob_object *blob_deriv,
    struct got_object_id *deriv_base_commit_id,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg)
{
	const struct got_error *err = NULL;
	int merged_fd = -1;
	FILE *f_deriv = NULL, *f_orig = NULL;
	char *blob_deriv_path = NULL, *blob_orig_path = NULL;
	char *merged_path = NULL, *base_path = NULL;
	char *id_str = NULL;
	char *label_deriv = NULL;
	int overlapcnt = 0;
	char *parent;

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

	err = got_object_id_str(&id_str, deriv_base_commit_id);
	if (err)
		goto done;
	if (asprintf(&label_deriv, "commit %s", id_str) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}

	err = got_merge_diff3(&overlapcnt, merged_fd, blob_deriv_path,
	    blob_orig_path, ondisk_path, label_deriv, path);
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
		err = check_files_equal(local_changes_subsumed, blob_deriv_path,
		    merged_path);
		if (err)
			goto done;
	}

	if (chmod(merged_path, st_mode) != 0) {
		err = got_error_from_errno2("chmod", merged_path);
		goto done;
	}

	if (rename(merged_path, ondisk_path) != 0) {
		err = got_error_from_errno3("rename", merged_path,
		    ondisk_path);
		unlink(merged_path);
		goto done;
	}

done:
	if (merged_fd != -1 && close(merged_fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	if (f_deriv && fclose(f_deriv) != 0 && err == NULL)
		err = got_error_from_errno("fclose");
	if (f_orig && fclose(f_orig) != 0 && err == NULL)
		err = got_error_from_errno("fclose");
	free(merged_path);
	free(base_path);
	if (blob_deriv_path) {
		unlink(blob_deriv_path);
		free(blob_deriv_path);
	}
	if (blob_orig_path) {
		unlink(blob_orig_path);
		free(blob_orig_path);
	}
	free(id_str);
	free(label_deriv);
	return err;
}

static const struct got_error *
update_blob_fileindex_entry(struct got_worktree *worktree,
    struct got_fileindex *fileindex, struct got_fileindex_entry *ie,
    const char *ondisk_path, const char *path, struct got_blob_object *blob,
    int update_timestamps)
{
	const struct got_error *err = NULL;

	if (ie == NULL)
		ie = got_fileindex_entry_get(fileindex, path);
	if (ie)
		err = got_fileindex_entry_update(ie, ondisk_path,
		    blob->id.sha1, worktree->base_commit_id->sha1,
		    update_timestamps);
	else {
		struct got_fileindex_entry *new_ie;
		err = got_fileindex_entry_alloc(&new_ie, ondisk_path,
		    path, blob->id.sha1, worktree->base_commit_id->sha1);
		if (!err)
			err = got_fileindex_entry_add(fileindex, new_ie);
	}
	return err;
}

static const struct got_error *
install_blob(struct got_worktree *worktree, const char *ondisk_path,
   const char *path, uint16_t te_mode, uint16_t st_mode,
   struct got_blob_object *blob, int restoring_missing_file,
   int reverting_versioned_file, struct got_repository *repo,
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
			if (!S_ISREG(st_mode)) {
				/* TODO file is obstructed; do something */
				err = got_error(GOT_ERR_FILE_OBSTRUCTED);
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

	if (restoring_missing_file)
		err = (*progress_cb)(progress_arg, GOT_STATUS_MISSING, path);
	else if (reverting_versioned_file)
		err = (*progress_cb)(progress_arg, GOT_STATUS_REVERT, path);
	else
		err = (*progress_cb)(progress_arg,
		    update ? GOT_STATUS_UPDATE : GOT_STATUS_ADD, path);
	if (err)
		goto done;

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

	if (te_mode & S_IXUSR) {
		if (chmod(ondisk_path, st_mode | S_IXUSR) == -1) {
			err = got_error_from_errno2("chmod", ondisk_path);
			goto done;
		}
	} else {
		if (chmod(ondisk_path, st_mode & ~S_IXUSR) == -1) {
			err = got_error_from_errno2("chmod", ondisk_path);
			goto done;
		}
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
stat_info_differs(struct got_fileindex_entry *ie, struct stat *sb)
{
	return !(ie->ctime_sec == sb->st_ctime &&
	    ie->ctime_nsec == sb->st_ctimensec &&
	    ie->mtime_sec == sb->st_mtime &&
	    ie->mtime_nsec == sb->st_mtimensec &&
	    ie->size == (sb->st_size & 0xffffffff));
}

static const struct got_error *
get_file_status(unsigned char *status, struct stat *sb,
    struct got_fileindex_entry *ie, const char *abspath,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object_id id;
	size_t hdrlen;
	FILE *f = NULL;
	uint8_t fbuf[8192];
	struct got_blob_object *blob = NULL;
	size_t flen, blen;

	*status = GOT_STATUS_NO_CHANGE;

	if (lstat(abspath, sb) == -1) {
		if (errno == ENOENT) {
			if (ie) {
				if (got_fileindex_entry_has_file_on_disk(ie))
					*status = GOT_STATUS_MISSING;
				else
					*status = GOT_STATUS_DELETE;
				sb->st_mode =
				    ((ie->mode >> GOT_FILEIDX_MODE_PERMS_SHIFT)
				    & (S_IRWXU | S_IRWXG | S_IRWXO));
			} else
				sb->st_mode = GOT_DEFAULT_FILE_MODE;
			return NULL;
		}
		return got_error_from_errno2("lstat", abspath);
	}

	if (!S_ISREG(sb->st_mode)) {
		*status = GOT_STATUS_OBSTRUCTED;
		return NULL;
	}

	if (ie == NULL)
		return NULL;

	if (!got_fileindex_entry_has_file_on_disk(ie)) {
		*status = GOT_STATUS_DELETE;
		return NULL;
	} else if (!got_fileindex_entry_has_blob(ie)) {
		*status = GOT_STATUS_ADD;
		return NULL;
	}

	if (!stat_info_differs(ie, sb))
		return NULL;

	memcpy(id.sha1, ie->blob_sha1, sizeof(id.sha1));
	err = got_object_open_as_blob(&blob, repo, &id, sizeof(fbuf));
	if (err)
		return err;

	f = fopen(abspath, "r");
	if (f == NULL) {
		err = got_error_from_errno2("fopen", abspath);
		goto done;
	}
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
	}
done:
	if (blob)
		got_object_blob_close(blob);
	if (f)
		fclose(f);
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

	err = get_file_status(&status, &sb, ie, ondisk_path, repo);
	if (err)
		goto done;

	if (status == GOT_STATUS_OBSTRUCTED) {
		err = (*progress_cb)(progress_arg, status, path);
		goto done;
	}

	if (ie && status != GOT_STATUS_MISSING) {
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
		    memcmp(ie->blob_sha1, te->id->sha1,
		    SHA1_DIGEST_LENGTH) == 0) {
			err = sync_timestamps(ondisk_path, status, ie, &sb);
			goto done;
		}
	}

	err = got_object_open_as_blob(&blob, repo, te->id, 8192);
	if (err)
		goto done;

	if (status == GOT_STATUS_MODIFY || status == GOT_STATUS_ADD) {
		int update_timestamps;
		struct got_blob_object *blob2 = NULL;
		if (got_fileindex_entry_has_blob(ie)) {
			struct got_object_id id2;
			memcpy(id2.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
			err = got_object_open_as_blob(&blob2, repo, &id2, 8192);
			if (err)
				goto done;
		}
		err = merge_blob(&update_timestamps, worktree, blob2,
		    ondisk_path, path, sb.st_mode, blob,
		    worktree->base_commit_id, repo,
		    progress_cb, progress_arg);
		if (blob2)
			got_object_blob_close(blob2);
		/*
		 * Do not update timestamps of files with local changes.
		 * Otherwise, a future status walk would treat them as
		 * unmodified files again.
		 */
		err = got_fileindex_entry_update(ie, ondisk_path,
		    blob->id.sha1, worktree->base_commit_id->sha1,
		    update_timestamps);
	} else if (status == GOT_STATUS_DELETE) {
		err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE, path);
		if (err)
			goto done;
		err = update_blob_fileindex_entry(worktree, fileindex, ie,
		    ondisk_path, path, blob, 0);
		if (err)
			goto done;
	} else {
		err = install_blob(worktree, ondisk_path, path, te->mode,
		    sb.st_mode, blob, status == GOT_STATUS_MISSING, 0,
		    repo, progress_cb, progress_arg);
		if (err)
			goto done;
		err = update_blob_fileindex_entry(worktree, fileindex, ie,
		    ondisk_path, path, blob, 1);
		if (err)
			goto done;
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

	if (asprintf(&ondisk_path, "%s/%s", worktree->root_path, ie->path)
	    == -1)
		return got_error_from_errno("asprintf");

	err = get_file_status(&status, &sb, ie, ondisk_path, repo);
	if (err)
		return err;

	if (status == GOT_STATUS_MODIFY || status == GOT_STATUS_CONFLICT ||
	    status == GOT_STATUS_ADD) {
		err = (*progress_cb)(progress_arg, GOT_STATUS_MERGE, ie->path);
		if (err)
			return err;
		/*
		 * Preserve the working file and change the deleted blob's
		 * entry into a schedule-add entry.
		 */
		err = got_fileindex_entry_update(ie, ondisk_path, NULL, NULL,
		    0);
		if (err)
			return err;
	} else {
		err = (*progress_cb)(progress_arg, GOT_STATUS_DELETE, ie->path);
		if (err)
			return err;
		if (status == GOT_STATUS_NO_CHANGE) {
			err = remove_ondisk_file(worktree->root_path, ie->path);
			if (err)
				return err;
		}
		got_fileindex_entry_remove(fileindex, ie);
	}

	return err;
}

struct diff_cb_arg {
    struct got_fileindex *fileindex;
    struct got_worktree *worktree;
    struct got_repository *repo;
    got_worktree_checkout_cb progress_cb;
    void *progress_arg;
    got_worktree_cancel_cb cancel_cb;
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

static const struct got_error *
get_ref_name(char **refname, struct got_worktree *worktree, const char *prefix)
{
	const struct got_error *err = NULL;
	char *uuidstr = NULL;
	uint32_t uuid_status;

	*refname = NULL;

	uuid_to_string(&worktree->uuid, &uuidstr, &uuid_status);
	if (uuid_status != uuid_s_ok)
		return got_error_uuid(uuid_status);

	if (asprintf(refname, "%s-%s", prefix, uuidstr)
	    == -1) {
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
got_worktree_get_histedit_list_path(char **path, struct got_worktree *worktree)
{
	if (asprintf(path, "%s/%s/%s", worktree->root_path,
	    GOT_WORKTREE_GOT_DIR, GOT_WORKTREE_HISTEDIT_LIST) == -1) {
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
    void *progress_arg, got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_commit_object *commit = NULL;
	struct got_tree_object *tree = NULL;
	struct got_fileindex_diff_tree_cb diff_cb;
	struct diff_cb_arg arg;

	err = ref_base_commit(worktree, repo);
	if (err)
		goto done;

	err = got_object_open_as_commit(&commit, repo,
	   worktree->base_commit_id);
	if (err)
		goto done;

	err = got_object_open_as_tree(&tree, repo, tree_id);
	if (err)
		goto done;

	if (entry_name &&
	    got_object_tree_find_entry(tree, entry_name) == NULL) {
		err = got_error(GOT_ERR_NO_TREE_ENTRY);
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
got_worktree_checkout_files(struct got_worktree *worktree, const char *path,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg, got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_commit_object *commit = NULL;
	struct got_object_id *tree_id = NULL;
	struct got_tree_object *tree = NULL;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	char *relpath = NULL, *entry_name = NULL;
	int entry_type;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = find_tree_entry_for_checkout(&entry_type, &relpath, &tree_id,
	    path, worktree, repo);
	if (err)
		goto done;

	if (entry_type == GOT_OBJ_TYPE_BLOB) {
		entry_name = basename(path);
		if (entry_name == NULL) {
			err = got_error_from_errno2("basename", path);
			goto done;
		}
	}

	/*
	 * Read the file index.
	 * Checking out files is supposed to be an idempotent operation.
	 * If the on-disk file index is incomplete we will try to complete it.
	 */
	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	err = checkout_files(worktree, fileindex, relpath, tree_id, entry_name,
	    repo, progress_cb, progress_arg, cancel_cb, cancel_arg);
	if (err == NULL) {
		struct bump_base_commit_id_arg bbc_arg;
		bbc_arg.base_commit_id = worktree->base_commit_id;
		bbc_arg.entry_name = entry_name;
		bbc_arg.path = path;
		bbc_arg.path_len = strlen(path);
		bbc_arg.progress_cb = progress_cb;
		bbc_arg.progress_arg = progress_arg;
		err = got_fileindex_for_each_entry_safe(fileindex,
		    bump_base_commit_id, &bbc_arg);
	}
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	free(fileindex_path);
	free(relpath);
	if (tree)
		got_object_tree_close(tree);
	if (commit)
		got_object_commit_close(commit);
	if (fileindex)
		got_fileindex_free(fileindex);
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
    got_worktree_cancel_cb cancel_cb;
    void *cancel_arg;
    struct got_object_id *commit_id2;
};

static const struct got_error *
merge_file_cb(void *arg, struct got_blob_object *blob1,
    struct got_blob_object *blob2, struct got_object_id *id1,
    struct got_object_id *id2, const char *path1, const char *path2,
    struct got_repository *repo)
{
	static const struct got_error *err = NULL;
	struct merge_file_cb_arg *a = arg;
	struct got_fileindex_entry *ie;
	char *ondisk_path = NULL;
	struct stat sb;
	unsigned char status;
	int local_changes_subsumed;

	if (blob1 && blob2) {
		ie = got_fileindex_entry_get(a->fileindex, path2);
		if (ie == NULL)
			return (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_MISSING, path2);

		if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
		    path2) == -1)
			return got_error_from_errno("asprintf");

		err = get_file_status(&status, &sb, ie, ondisk_path, repo);
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

		err = merge_blob(&local_changes_subsumed, a->worktree, blob1,
		    ondisk_path, path2, sb.st_mode, blob2, a->commit_id2, repo,
		    a->progress_cb, a->progress_arg);
	} else if (blob1) {
		ie = got_fileindex_entry_get(a->fileindex, path1);
		if (ie == NULL)
			return (*a->progress_cb)(a->progress_arg,
			    GOT_STATUS_MISSING, path2);

		if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path,
		    path1) == -1)
			return got_error_from_errno("asprintf");

		err = get_file_status(&status, &sb, ie, ondisk_path, repo);
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
		case GOT_STATUS_ADD:
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
		ie = got_fileindex_entry_get(a->fileindex, path2);
		if (ie) {
			err = get_file_status(&status, &sb, ie, ondisk_path,
			    repo);
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
			err = merge_blob(&local_changes_subsumed, a->worktree,
			    NULL, ondisk_path, path2, sb.st_mode, blob2,
			    a->commit_id2, repo,
			    a->progress_cb, a->progress_arg);
			if (status == GOT_STATUS_DELETE) {
				err = update_blob_fileindex_entry(a->worktree,
				    a->fileindex, ie, ondisk_path, ie->path,
				    blob2, 0);
				if (err)
					goto done;
			}
		} else {
			sb.st_mode = GOT_DEFAULT_FILE_MODE;
			err = install_blob(a->worktree, ondisk_path, path2,
			    /* XXX get this from parent tree! */
			    GOT_DEFAULT_FILE_MODE,
			    sb.st_mode, blob2, 0, 0, repo,
			    a->progress_cb, a->progress_arg);
			if (err)
				goto done;
			err = got_fileindex_entry_alloc(&ie,
			    ondisk_path, path2, NULL, NULL);
			if (err)
				goto done;
			err = got_fileindex_entry_add(a->fileindex, ie);
			if (err) {
				got_fileindex_entry_free(ie);
				goto done;
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
	err = get_file_status(&status, &sb, ie, ondisk_path, a->repo);
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
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL, *sync_err;
	struct got_object_id *tree_id1 = NULL, *tree_id2 = NULL;
	struct got_tree_object *tree1 = NULL, *tree2 = NULL;
	struct merge_file_cb_arg arg;

	if (commit_id1) {
		err = got_object_id_by_path(&tree_id1, repo, commit_id1,
		    worktree->path_prefix);
		if (err)
			goto done;

		err = got_object_open_as_tree(&tree1, repo, tree_id1);
		if (err)
			goto done;
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
	arg.commit_id2 = commit_id2;
	err = got_diff_tree(tree1, tree2, "", "", repo, merge_file_cb, &arg);
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	if (tree1)
		got_object_tree_close(tree1);
	if (tree2)
		got_object_tree_close(tree2);
	return err;
}

const struct got_error *
got_worktree_merge_files(struct got_worktree *worktree,
    struct got_object_id *commit_id1, struct got_object_id *commit_id2,
    struct got_repository *repo, got_worktree_checkout_cb progress_cb,
    void *progress_arg, got_worktree_cancel_cb cancel_cb, void *cancel_arg)
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
    got_worktree_cancel_cb cancel_cb;
    void *cancel_arg;
};

static const struct got_error *
report_file_status(struct got_fileindex_entry *ie, const char *abspath,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	unsigned char status = GOT_STATUS_NO_CHANGE;
	struct stat sb;
	struct got_object_id blob_id, commit_id;

	err = get_file_status(&status, &sb, ie, abspath, repo);
	if (err == NULL && status != GOT_STATUS_NO_CHANGE) {
		memcpy(blob_id.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
		memcpy(commit_id.sha1, ie->commit_sha1, SHA1_DIGEST_LENGTH);
		err = (*status_cb)(status_arg, status, ie->path, &blob_id,
		    &commit_id);
	}
	return err;
}

static const struct got_error *
status_old_new(void *arg, struct got_fileindex_entry *ie,
    struct dirent *de, const char *parent_path)
{
	const struct got_error *err = NULL;
	struct diff_dir_cb_arg *a = arg;
	char *abspath;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (got_path_cmp(parent_path, a->status_path) != 0 &&
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

	err = report_file_status(ie, abspath, a->status_cb, a->status_arg,
	    a->repo);
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
	return (*a->status_cb)(a->status_arg, status, ie->path, &blob_id,
	    &commit_id);
}

static const struct got_error *
status_new(void *arg, struct dirent *de, const char *parent_path)
{
	const struct got_error *err = NULL;
	struct diff_dir_cb_arg *a = arg;
	char *path = NULL;

	if (a->cancel_cb && a->cancel_cb(a->cancel_arg))
		return got_error(GOT_ERR_CANCELLED);

	if (de->d_type == DT_DIR)
		return NULL;

	/* XXX ignore symlinks for now */
	if (de->d_type == DT_LNK)
		return NULL;

	if (parent_path[0]) {
		if (asprintf(&path, "%s/%s", parent_path, de->d_name) == -1)
			return got_error_from_errno("asprintf");
	} else {
		path = de->d_name;
	}

	if (got_path_is_child(path, a->status_path, a->status_path_len))
		err = (*a->status_cb)(a->status_arg, GOT_STATUS_UNVERSIONED,
		    path, NULL, NULL);
	if (parent_path[0])
		free(path);
	return err;
}

static const struct got_error *
worktree_status(struct got_worktree *worktree, const char *path,
    struct got_fileindex *fileindex, struct got_repository *repo,
    got_worktree_status_cb status_cb, void *status_arg,
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	DIR *workdir = NULL;
	struct got_fileindex_diff_dir_cb fdiff_cb;
	struct diff_dir_cb_arg arg;
	char *ondisk_path = NULL;

	if (asprintf(&ondisk_path, "%s%s%s",
	    worktree->root_path, path[0] ? "/" : "", path) == -1) {
		err = got_error_from_errno("asprintf");
		goto done;
	}
	workdir = opendir(ondisk_path);
	if (workdir == NULL) {
		if (errno == ENOTDIR || errno == ENOENT) {
			struct got_fileindex_entry *ie;
			ie = got_fileindex_entry_get(fileindex, path);
			if (ie == NULL)
				err = (*status_cb)(status_arg,
				    GOT_STATUS_UNVERSIONED, path, NULL, NULL);
			else
				err = report_file_status(ie, ondisk_path,
				    status_cb, status_arg, repo);
			if (err)
				goto done;
			err = report_file_status(ie, ondisk_path,
			    status_cb, status_arg, repo);
			goto done;
		} else {
			err = got_error_from_errno2("opendir", ondisk_path);
			goto done;
		}
	}
	fdiff_cb.diff_old_new = status_old_new;
	fdiff_cb.diff_old = status_old;
	fdiff_cb.diff_new = status_new;
	arg.fileindex = fileindex;
	arg.worktree = worktree;
	arg.status_path = path;
	arg.status_path_len = strlen(path);
	arg.repo = repo;
	arg.status_cb = status_cb;
	arg.status_arg = status_arg;
	arg.cancel_cb = cancel_cb;
	arg.cancel_arg = cancel_arg;
	err = got_fileindex_diff_dir(fileindex, workdir, worktree->root_path,
	    path, repo, &fdiff_cb, &arg);
done:
	if (workdir)
		closedir(workdir);
	free(ondisk_path);
	return err;
}

const struct got_error *
got_worktree_status(struct got_worktree *worktree,
    struct got_pathlist_head *paths, struct got_repository *repo,
    got_worktree_status_cb status_cb, void *status_arg,
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
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
			status_cb, status_arg, cancel_cb, cancel_arg);
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
	char *resolved, *cwd = NULL, *path = NULL;
	size_t len;

	*wt_path = NULL;

	resolved = realpath(arg, NULL);
	if (resolved == NULL) {
		if (errno != ENOENT)
			return got_error_from_errno2("realpath", arg);
		cwd = getcwd(NULL, 0);
		if (cwd == NULL)
			return got_error_from_errno("getcwd");
		if (asprintf(&resolved, "%s/%s", cwd, arg) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
	}

	if (strncmp(got_worktree_get_root_path(worktree), resolved,
	    strlen(got_worktree_get_root_path(worktree)))) {
		err = got_error(GOT_ERR_BAD_PATH);
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

static const struct got_error *
schedule_addition(const char *ondisk_path, struct got_fileindex *fileindex,
    const char *relpath, got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie;

	/* Re-adding an existing entry is a no-op. */
	if (got_fileindex_entry_get(fileindex, relpath) != NULL)
		return NULL;

	err = got_fileindex_entry_alloc(&ie, ondisk_path, relpath, NULL, NULL);
	if (err)
		return err;

	err = got_fileindex_entry_add(fileindex, ie);
	if (err) {
		got_fileindex_entry_free(ie);
		return err;
	}

	return report_file_status(ie, relpath, status_cb, status_arg, repo);
}

const struct got_error *
got_worktree_schedule_add(struct got_worktree *worktree,
    struct got_pathlist_head *ondisk_paths,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	TAILQ_FOREACH(pe, ondisk_paths, entry) {
		char *relpath;
		err = got_path_skip_common_ancestor(&relpath,
		    got_worktree_get_root_path(worktree), pe->path);
		if (err)
			break;
		err = schedule_addition(pe->path, fileindex, relpath,
		    status_cb, status_arg, repo);
		free(relpath);
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
schedule_for_deletion(const char *ondisk_path, struct got_fileindex *fileindex,
    const char *relpath, int delete_local_mods,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_fileindex_entry *ie = NULL;
	unsigned char status;
	struct stat sb;

	ie = got_fileindex_entry_get(fileindex, relpath);
	if (ie == NULL)
		return got_error(GOT_ERR_BAD_PATH);

	err = get_file_status(&status, &sb, ie, ondisk_path, repo);
	if (err)
		return err;

	if (status != GOT_STATUS_NO_CHANGE) {
		if (status == GOT_STATUS_DELETE)
			return got_error_set_errno(ENOENT, ondisk_path);
		if (status != GOT_STATUS_MODIFY)
			return got_error(GOT_ERR_FILE_STATUS);
		if (!delete_local_mods)
			return got_error(GOT_ERR_FILE_MODIFIED);
	}

	if (unlink(ondisk_path) != 0)
		return got_error_from_errno2("unlink", ondisk_path);

	got_fileindex_entry_mark_deleted_from_disk(ie);
	return report_file_status(ie, ondisk_path, status_cb, status_arg, repo);
}

const struct got_error *
got_worktree_schedule_delete(struct got_worktree *worktree,
    struct got_pathlist_head *ondisk_paths, int delete_local_mods,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *sync_err, *unlockerr;
	struct got_pathlist_entry *pe;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	TAILQ_FOREACH(pe, ondisk_paths, entry) {
		char *relpath;
		err = got_path_skip_common_ancestor(&relpath,
		    got_worktree_get_root_path(worktree), pe->path);
		if (err)
			break;
		err = schedule_for_deletion(pe->path, fileindex, relpath,
		    delete_local_mods, status_cb, status_arg, repo);
		free(relpath);
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
revert_file(struct got_worktree *worktree, struct got_fileindex *fileindex,
    const char *ondisk_path,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	char *relpath = NULL, *parent_path = NULL;
	struct got_fileindex_entry *ie;
	struct got_tree_object *tree = NULL;
	struct got_object_id *tree_id = NULL;
	const struct got_tree_entry *te;
	char *tree_path = NULL, *te_name;
	struct got_blob_object *blob = NULL;
	unsigned char status;
	struct stat sb;

	err = got_path_skip_common_ancestor(&relpath,
	    got_worktree_get_root_path(worktree), ondisk_path);
	if (err)
		goto done;

	ie = got_fileindex_entry_get(fileindex, relpath);
	if (ie == NULL) {
		err = got_error(GOT_ERR_BAD_PATH);
		goto done;
	}

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
	if (got_path_is_root_dir(worktree->path_prefix)) {
		tree_path = strdup(parent_path);
		if (tree_path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	} else {
		if (got_path_is_root_dir(parent_path)) {
			tree_path = strdup(worktree->path_prefix);
			if (tree_path == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		} else {
			if (asprintf(&tree_path, "%s/%s",
			    worktree->path_prefix, parent_path) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
	}

	err = got_object_id_by_path(&tree_id, repo, worktree->base_commit_id,
	    tree_path);
	if (err)
		goto done;

	err = got_object_open_as_tree(&tree, repo, tree_id);
	if (err)
		goto done;

	te_name = basename(ie->path);
	if (te_name == NULL) {
		err = got_error_from_errno2("basename", ie->path);
		goto done;
	}

	err = get_file_status(&status, &sb, ie, ondisk_path, repo);
	if (err)
		goto done;

	te = got_object_tree_find_entry(tree, te_name);
	if (te == NULL && status != GOT_STATUS_ADD) {
		err = got_error(GOT_ERR_NO_TREE_ENTRY);
		goto done;
	}

	switch (status) {
	case GOT_STATUS_ADD:
		err = (*progress_cb)(progress_arg, GOT_STATUS_REVERT, ie->path);
		if (err)
			goto done;
		got_fileindex_entry_remove(fileindex, ie);
		break;
	case GOT_STATUS_DELETE:
	case GOT_STATUS_MODIFY:
	case GOT_STATUS_CONFLICT:
	case GOT_STATUS_MISSING: {
		struct got_object_id id;
		memcpy(id.sha1, ie->blob_sha1, SHA1_DIGEST_LENGTH);
		err = got_object_open_as_blob(&blob, repo, &id, 8192);
		if (err)
			goto done;
		err = install_blob(worktree, ondisk_path, ie->path,
		    te->mode, sb.st_mode, blob, 0, 1, repo, progress_cb,
		    progress_arg);
		if (err)
			goto done;
		if (status == GOT_STATUS_DELETE) {
			err = update_blob_fileindex_entry(worktree,
			    fileindex, ie, ondisk_path, ie->path, blob, 1);
			if (err)
				goto done;
		}
		break;
	}
	default:
			goto done;
	}
done:
	free(relpath);
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
    struct got_pathlist_head *ondisk_paths,
    got_worktree_checkout_cb progress_cb, void *progress_arg,
    struct got_repository *repo)
{
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL;
	const struct got_error *err = NULL, *unlockerr = NULL;
	const struct got_error *sync_err = NULL;
	struct got_pathlist_entry *pe;

	err = lock_worktree(worktree, LOCK_EX);
	if (err)
		return err;

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	TAILQ_FOREACH(pe, ondisk_paths, entry) {
		err = revert_file(worktree, fileindex, pe->path,
		    progress_cb, progress_arg, repo);
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
	free(ct->base_commit_id);
	free(ct);
}

struct collect_commitables_arg {
	struct got_pathlist_head *commitable_paths;
	struct got_repository *repo;
	struct got_worktree *worktree;
};

static const struct got_error *
collect_commitables(void *arg, unsigned char status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *commit_id)
{
	struct collect_commitables_arg *a = arg;
	const struct got_error *err = NULL;
	struct got_commitable *ct = NULL;
	struct got_pathlist_entry *new = NULL;
	char *parent_path = NULL, *path = NULL;
	struct stat sb;

	if (status == GOT_STATUS_CONFLICT)
		return got_error(GOT_ERR_COMMIT_CONFLICT);

	if (status != GOT_STATUS_MODIFY && status != GOT_STATUS_ADD &&
	    status != GOT_STATUS_DELETE)
		return NULL;

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
	if (status == GOT_STATUS_DELETE) {
		sb.st_mode = GOT_DEFAULT_FILE_MODE;
	} else {
		if (lstat(ct->ondisk_path, &sb) != 0) {
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

	ct->status = status;
	ct->blob_id = NULL; /* will be filled in when blob gets created */
	if (ct->status != GOT_STATUS_ADD) {
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

static const struct got_error *write_tree(struct got_object_id **,
    struct got_tree_object *, const char *, struct got_pathlist_head *,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *);

static const struct got_error *
write_subtree(struct got_object_id **new_subtree_id,
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

	err = got_object_open_as_tree(&subtree, repo, te->id);
	if (err)
		return err;

	err = write_tree(new_subtree_id, subtree, subpath, commitable_paths,
	    status_cb, status_arg, repo);
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

	free((*new_te)->id);
	(*new_te)->id = got_object_id_dup(ct->blob_id);
	if ((*new_te)->id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
done:
	if (err && *new_te) {
		got_object_tree_entry_close(*new_te);
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
	(*new_te)->name = strdup(ct_name);
	if ((*new_te)->name == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}

	(*new_te)->mode = get_ct_file_mode(ct);

	(*new_te)->id = got_object_id_dup(ct->blob_id);
	if ((*new_te)->id == NULL) {
		err = got_error_from_errno("got_object_id_dup");
		goto done;
	}
done:
	if (err && *new_te) {
		got_object_tree_entry_close(*new_te);
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
	while (ct_path[0] == '/')
		ct_path++;
	return (*status_cb)(status_arg, ct->status, ct_path, ct->blob_id, NULL);
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

		if (ct->status != GOT_STATUS_MODIFY &&
		    ct->status != GOT_STATUS_DELETE)
			continue;

		if (got_object_id_cmp(ct->base_blob_id, te->id) != 0)
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

	*new_tep = NULL;

	if (asprintf(&subtree_path, "%s%s%s", path_base_tree,
	    got_path_is_root_dir(path_base_tree) ? "" : "/",
	    child_path) == -1)
		return got_error_from_errno("asprintf");

	new_te = calloc(1, sizeof(*new_te));
	new_te->mode = S_IFDIR;
	new_te->name = strdup(child_path);
	if (new_te->name == NULL) {
		err = got_error_from_errno("strdup");
		got_object_tree_entry_close(new_te);
		goto done;
	}
	err = write_tree(&new_te->id, NULL, subtree_path,
	    commitable_paths, status_cb, status_arg, repo);
	if (err) {
		got_object_tree_entry_close(new_te);
		goto done;
	}
done:
	free(subtree_path);
	if (err == NULL)
		*new_tep = new_te;
	return err;
}

static const struct got_error *
write_tree(struct got_object_id **new_tree_id,
    struct got_tree_object *base_tree, const char *path_base_tree,
    struct got_pathlist_head *commitable_paths,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	const struct got_tree_entries *base_entries = NULL;
	struct got_pathlist_head paths;
	struct got_tree_entries new_tree_entries;
	struct got_tree_entry *te, *new_te = NULL;
	struct got_pathlist_entry *pe;

	TAILQ_INIT(&paths);
	new_tree_entries.nentries = 0;
	SIMPLEQ_INIT(&new_tree_entries.head);

	/* Insert, and recurse into, newly added entries first. */
	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		char *child_path = NULL, *slash;

		if (ct->status != GOT_STATUS_ADD ||
		    (ct->flags & GOT_COMMITABLE_ADDED))
			continue;

		 if (!got_path_is_child(pe->path, path_base_tree,
		     strlen(path_base_tree)))
			continue;

		err = got_path_skip_common_ancestor(&child_path, path_base_tree,
		    pe->path);
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
			}
		}
	}

	if (base_tree) {
		/* Handle modified and deleted entries. */
		base_entries = got_object_tree_get_entries(base_tree);
		SIMPLEQ_FOREACH(te, &base_entries->head, entry) {
			struct got_commitable *ct = NULL;

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
					free(new_te->id);
					err = write_subtree(&new_te->id, te,
					    path_base_tree, commitable_paths,
					    status_cb, status_arg, repo);
					if (err)
						goto done;
				}
				err = insert_tree_entry(new_te, &paths);
				if (err)
					goto done;
				continue;
			}

			err = match_deleted_or_modified_ct(&ct, te,
			    path_base_tree, commitable_paths);
			if (ct) {
				/* NB: Deleted entries get dropped here. */
				if (ct->status == GOT_STATUS_MODIFY) {
					err = alloc_modified_blob_tree_entry(
					    &new_te, te, ct);
					if (err)
						goto done;
					err = insert_tree_entry(new_te, &paths);
					if (err)
						goto done;
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
			}
		}
	}

	/* Write new list of entries; deleted entries have been dropped. */
	TAILQ_FOREACH(pe, &paths, entry) {
		struct got_tree_entry *te = pe->data;
		new_tree_entries.nentries++;
		SIMPLEQ_INSERT_TAIL(&new_tree_entries.head, te, entry);
	}
	err = got_object_tree_create(new_tree_id, &new_tree_entries, repo);
done:
	got_object_tree_entries_close(&new_tree_entries);
	got_pathlist_free(&paths);
	return err;
}

static const struct got_error *
update_fileindex_after_commit(struct got_pathlist_head *commitable_paths,
    struct got_object_id *new_base_commit_id, struct got_fileindex *fileindex)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;

	TAILQ_FOREACH(pe, commitable_paths, entry) {
		struct got_fileindex_entry *ie;
		struct got_commitable *ct = pe->data;

		ie = got_fileindex_entry_get(fileindex, pe->path);
		if (ie) {
			if (ct->status == GOT_STATUS_DELETE) {
				got_fileindex_entry_remove(fileindex, ie);
				got_fileindex_entry_free(ie);
			} else
				err = got_fileindex_entry_update(ie,
				    ct->ondisk_path, ct->blob_id->sha1,
				    new_base_commit_id->sha1, 1);
		} else {
			err = got_fileindex_entry_alloc(&ie,
			    ct->ondisk_path, pe->path, ct->blob_id->sha1,
			    new_base_commit_id->sha1);
			if (err)
				break;
			err = got_fileindex_entry_add(fileindex, ie);
			if (err)
				break;
		}
	}
	return err;
}

static const struct got_error *
check_ct_out_of_date(struct got_commitable *ct, struct got_repository *repo,
	struct got_object_id *head_commit_id)
{
	const struct got_error *err = NULL;
	struct got_object_id *id_in_head = NULL, *id = NULL;
	struct got_commit_object *commit = NULL;
	char *path = NULL;
	const char *ct_path = ct->in_repo_path;

	while (ct_path[0] == '/')
		ct_path++;

	/*
	 * Ensure that no modifications were made to files *and their parents*
	 * in commits between the file's base commit and the branch head.
	 *
	 * Checking the parents is important for detecting conflicting tree
	 * configurations (files or parent folders might have been moved,
	 * deleted, added again, etc.). Such changes need to be merged with
	 * local changes before a commit can occur.
	 *
	 * The implication is that the file's (parent) entry in the root
	 * directory must have the same ID in all relevant commits.
	 */
	if (ct->status != GOT_STATUS_ADD) {
		struct got_object_qid *pid;
		char *slash;
		struct got_object_id *root_entry_id = NULL;

		/* Trivial case: base commit == head commit */
		if (got_object_id_cmp(ct->base_commit_id, head_commit_id) == 0)
			return NULL;

		/* Compute the path to the root directory's entry. */
		path = strdup(ct_path);
		if (path == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
		slash = strchr(path, '/');
		if (slash)
			*slash = '\0';

		err = got_object_open_as_commit(&commit, repo, head_commit_id);
		if (err)
			goto done;

		err = got_object_id_by_path(&root_entry_id, repo,
		    head_commit_id, path);
		if (err)
			goto done;

		pid = SIMPLEQ_FIRST(got_object_commit_get_parent_ids(commit));
		while (pid) {
			struct got_commit_object *pcommit;

			err = got_object_id_by_path(&id, repo, pid->id, path);
			if (err) {
				if (err->code != GOT_ERR_NO_TREE_ENTRY)
					goto done;
				err = NULL;
				break;
			}

			err = got_object_id_by_path(&id, repo, pid->id, path);
			if (err)
				goto done;

			if (got_object_id_cmp(id, root_entry_id) != 0) {
				err = got_error(GOT_ERR_COMMIT_OUT_OF_DATE);
				break;
			}

			if (got_object_id_cmp(pid->id, ct->base_commit_id) == 0)
				break; /* all relevant commits scanned */

			err = got_object_open_as_commit(&pcommit, repo,
			    pid->id);
			if (err)
				goto done;

			got_object_commit_close(commit);
			commit = pcommit;
			pid = SIMPLEQ_FIRST(got_object_commit_get_parent_ids(
			    commit));
		}
	} else {
		/* Require that added files don't exist in the branch head. */
		err = got_object_id_by_path(&id_in_head, repo, head_commit_id,
		    ct_path);
		if (err && err->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
		err = id_in_head ? got_error(GOT_ERR_COMMIT_OUT_OF_DATE) : NULL;
	}
done:
	if (commit)
		got_object_commit_close(commit);
	free(id_in_head);
	free(id);
	free(path);
	return err;
}

const struct got_error *
commit_worktree(struct got_object_id **new_commit_id,
    struct got_pathlist_head *commitable_paths,
    struct got_object_id *head_commit_id, struct got_worktree *worktree,
    const char *ondisk_path, const char *author, const char *committer,
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

		if (ct->status != GOT_STATUS_ADD &&
		    ct->status != GOT_STATUS_MODIFY)
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
	err = write_tree(&new_tree_id, head_tree, "/", commitable_paths,
	    status_cb, status_arg, repo);
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

const struct got_error *
got_worktree_commit(struct got_object_id **new_commit_id,
    struct got_worktree *worktree, const char *ondisk_path,
    const char *author, const char *committer,
    got_worktree_commit_msg_cb commit_msg_cb, void *commit_arg,
    got_worktree_status_cb status_cb, void *status_arg,
    struct got_repository *repo)
{
	const struct got_error *err = NULL, *unlockerr = NULL, *sync_err;
	struct got_fileindex *fileindex = NULL;
	char *fileindex_path = NULL, *relpath = NULL;
	struct got_pathlist_head commitable_paths;
	struct collect_commitables_arg cc_arg;
	struct got_pathlist_entry *pe;
	struct got_reference *head_ref = NULL;
	struct got_object_id *head_commit_id = NULL;

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

	if (ondisk_path) {
		if (strcmp(ondisk_path, worktree->root_path) == 0) {
			relpath = strdup("");
			if (relpath == NULL) {
				err = got_error_from_errno("strdup");
				goto done;
			}
		} else {
			err = got_path_skip_common_ancestor(&relpath,
			    worktree->root_path, ondisk_path);
			if (err)
				return err;
		}
	}

	err = open_fileindex(&fileindex, &fileindex_path, worktree);
	if (err)
		goto done;

	cc_arg.commitable_paths = &commitable_paths;
	cc_arg.worktree = worktree;
	cc_arg.repo = repo;
	err = worktree_status(worktree, relpath ? relpath : "",
	    fileindex, repo, collect_commitables, &cc_arg, NULL, NULL);
	if (err)
		goto done;

	if (TAILQ_EMPTY(&commitable_paths)) {
		err = got_error(GOT_ERR_COMMIT_NO_CHANGES);
		goto done;
	}

	TAILQ_FOREACH(pe, &commitable_paths, entry) {
		struct got_commitable *ct = pe->data;
		err = check_ct_out_of_date(ct, repo, head_commit_id);
		if (err)
			goto done;
	}

	err = commit_worktree(new_commit_id, &commitable_paths,
	    head_commit_id, worktree, ondisk_path, author, committer,
	    commit_msg_cb, commit_arg, status_cb, status_arg, repo);
	if (err)
		goto done;

	err = update_fileindex_after_commit(&commitable_paths, *new_commit_id,
	    fileindex);
	sync_err = sync_fileindex(fileindex, fileindex_path);
	if (sync_err && err == NULL)
		err = sync_err;
done:
	if (fileindex)
		got_fileindex_free(fileindex);
	free(fileindex_path);
	free(relpath);
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
	int rebase_in_progress;
};

static const struct got_error *
check_rebase_ok(void *arg, struct got_fileindex_entry *ie)
{
	const struct got_error *err = NULL;
	struct check_rebase_ok_arg *a = arg;
	unsigned char status;
	struct stat sb;
	char *ondisk_path;

	if (!a->rebase_in_progress) {
		/* Reject rebase of a work tree with mixed base commits. */
		if (memcmp(ie->commit_sha1, a->worktree->base_commit_id->sha1,
		    SHA1_DIGEST_LENGTH))
			return got_error(GOT_ERR_MIXED_COMMITS);
	}

	if (asprintf(&ondisk_path, "%s/%s", a->worktree->root_path, ie->path)
	    == -1)
		return got_error_from_errno("asprintf");

	/* Reject rebase of a work tree with modified or conflicted files. */
	err = get_file_status(&status, &sb, ie, ondisk_path, a->repo);
	free(ondisk_path);
	if (err)
		return err;

	if (a->rebase_in_progress) {
		if (status == GOT_STATUS_CONFLICT)
			return got_error(GOT_ERR_CONFLICTS);
	} else if (status != GOT_STATUS_NO_CHANGE)
		return got_error(GOT_ERR_MODIFIED);

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
	ok_arg.rebase_in_progress = 0;
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

	err = get_rebase_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		return err;

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
rebase_status(void *arg, unsigned char status, const char *path,
    struct got_object_id *blob_id, struct got_object_id *commit_id)
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
    struct got_repository *repo)
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
	} else {
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
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
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
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	char *commit_ref_name;

	err = get_rebase_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = store_commit_id(commit_ref_name, commit_id, repo);
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
    got_worktree_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	char *commit_ref_name;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = store_commit_id(commit_ref_name, commit_id, repo);
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
	/*
	 * If possible get the status of individual files directly to
	 * avoid crawling the entire work tree once per rebased commit.
	 * TODO: Ideally, merged_paths would contain a list of commitables
	 * we could use so we could skip worktree_status() entirely.
	 */
	if (merged_paths) {
		struct got_pathlist_entry *pe;
		if (TAILQ_EMPTY(merged_paths)) {
			err = got_error(GOT_ERR_NO_MERGED_PATHS);
			goto done;
		}
		TAILQ_FOREACH(pe, merged_paths, entry) {
			err = worktree_status(worktree, pe->path, fileindex,
			    repo, collect_commitables, &cc_arg, NULL, NULL);
			if (err)
				goto done;
		}
	} else {
		err = worktree_status(worktree, "", fileindex, repo,
		    collect_commitables, &cc_arg, NULL, NULL);
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

	if (new_logmsg)
		logmsg = strdup(new_logmsg);
	else
		logmsg = strdup(got_object_commit_get_logmsg(orig_commit));
	if (logmsg == NULL)
		return got_error_from_errno("strdup");

	err = commit_worktree(new_commit_id, &commitable_paths, head_commit_id,
	    worktree, NULL, got_object_commit_get_author(orig_commit),
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
	    fileindex);
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
	struct got_object_id *commit_id = NULL;

	err = get_histedit_commit_ref_name(&commit_ref_name, worktree);
	if (err)
		return err;

	err = got_ref_open(&commit_ref, repo, commit_ref_name, 0);
	if (err)
		goto done;
	err = got_ref_resolve(&commit_id, repo, commit_ref);
	if (err)
		goto done;
	if (got_object_id_cmp(commit_id, orig_commit_id) != 0) {
		err = got_error(GOT_ERR_HISTEDIT_COMMITID);
		goto done;
	}

	err = rebase_commit(new_commit_id, merged_paths, commit_ref,
	    worktree, fileindex, tmp_branch, orig_commit, new_logmsg, repo);
done:
	if (commit_ref)
		got_ref_close(commit_ref);
	free(commit_ref_name);
	free(commit_id);
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

struct collect_revertible_paths_arg {
	struct got_pathlist_head *revertible_paths;
	struct got_worktree *worktree;
};

static const struct got_error *
collect_revertible_paths(void *arg, unsigned char status, const char *relpath,
    struct got_object_id *blob_id, struct got_object_id *commit_id)
{
	struct collect_revertible_paths_arg *a = arg;
	const struct got_error *err = NULL;
	struct got_pathlist_entry *new = NULL;
	char *path = NULL;

	if (status != GOT_STATUS_ADD &&
	    status != GOT_STATUS_DELETE &&
	    status != GOT_STATUS_MODIFY &&
	    status != GOT_STATUS_CONFLICT &&
	    status != GOT_STATUS_MISSING)
		return NULL;

	if (asprintf(&path, "%s/%s", a->worktree->root_path, relpath) == -1)
		return got_error_from_errno("asprintf");

	err = got_pathlist_insert(&new, a->revertible_paths, path, NULL);
	if (err || new == NULL)
		free(path);
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
	struct got_pathlist_head revertible_paths;
	struct got_pathlist_entry *pe;
	struct collect_revertible_paths_arg crp_arg;
	struct got_object_id *tree_id = NULL;

	TAILQ_INIT(&revertible_paths);

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

	crp_arg.revertible_paths = &revertible_paths;
	crp_arg.worktree = worktree;
	err = worktree_status(worktree, "", fileindex, repo,
	    collect_revertible_paths, &crp_arg, NULL, NULL);
	if (err)
		goto done;

	TAILQ_FOREACH(pe, &revertible_paths, entry) {
		err = revert_file(worktree, fileindex, pe->path,
		    progress_cb, progress_arg, repo);
		if (err)
			goto sync;
	}

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
	TAILQ_FOREACH(pe, &revertible_paths, entry)
		free((char *)pe->path);
	got_pathlist_free(&revertible_paths);

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
	ok_arg.rebase_in_progress = 0;
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

	err = get_histedit_tmp_ref_name(&tmp_branch_name, worktree);
	if (err)
		return err;

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
	struct got_pathlist_head revertible_paths;
	struct got_pathlist_entry *pe;
	struct collect_revertible_paths_arg crp_arg;
	struct got_object_id *tree_id = NULL;

	TAILQ_INIT(&revertible_paths);

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

	crp_arg.revertible_paths = &revertible_paths;
	crp_arg.worktree = worktree;
	err = worktree_status(worktree, "", fileindex, repo,
	    collect_revertible_paths, &crp_arg, NULL, NULL);
	if (err)
		goto done;

	TAILQ_FOREACH(pe, &revertible_paths, entry) {
		err = revert_file(worktree, fileindex, pe->path,
		    progress_cb, progress_arg, repo);
		if (err)
			goto sync;
	}

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
	TAILQ_FOREACH(pe, &revertible_paths, entry)
		free((char *)pe->path);
	got_pathlist_free(&revertible_paths);

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

	err = store_commit_id(commit_ref_name, commit_id, repo);
	if (err)
		goto done;

	err = delete_ref(commit_ref_name, repo);
done:
	free(commit_ref_name);
	return err;
}
