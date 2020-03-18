/*
 * Copyright (c) 2019 Ori Bernstein <ori@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#include <stdint.h>
#include <errno.h>
#include <imsg.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sha1.h>
#include <fcntl.h>
#include <zlib.h>
#include <err.h>

#include "got_error.h"
#include "got_object.h"

#include "got_lib_sha1.h"
#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_privsep.h"

#define GOT_PKTMAX	65536

struct got_object *indexed;
static int chattygit;
static char *fetchbranch;
static char *upstream = "origin";
static struct got_object_id zhash = {.sha1={0}};

static char*
strip(char *ref)
{
	return ref;
}

int
readn(int fd, void *buf, size_t n)
{
	ssize_t r, off;

	off = 0;
	while (off != n) {
		r = read(fd, buf + off, n - off);
		if (r < 0)
			return -1;
		if (r == 0)
			return off;
		off += r;
	}
	return off;
}

int
flushpkt(int fd)
{
	if(chattygit)
		fprintf(stderr, "writepkt: 0000\n");
	return write(fd, "0000", 4);
}


int
readpkt(int fd, char *buf, int nbuf)
{
	char len[5];
	char *e;
	int n, r;

	if(readn(fd, len, 4) == -1){
		return -1;
	}
	len[4] = 0;
	n = strtol(len, &e, 16);
	if(n == 0){
		if(chattygit)
			fprintf(stderr, "readpkt: 0000\n");
		return 0;
	}
	if(e != len + 4 || n <= 4)
		err(1, "invalid packet line length");
	n  -= 4;
	if(n >= nbuf)
		err(1, "buffer too small");
	if((r = readn(fd, buf, n)) != n)
		return -1;
	buf[n] = 0;
	if(chattygit)
		fprintf(stderr, "readpkt: %s:\t%.*s\n", len, nbuf, buf);
	return n;
}

int
writepkt(int fd, char *buf, int nbuf)
{
	char len[5];
	int i;

	if (snprintf(len, sizeof(len), "%04x", nbuf + 4) >= sizeof(len))
		return -1;
	if(write(fd, len, 4) != 4)
		return -1;
	if(write(fd, buf, nbuf) != nbuf)
		return -1;
	if(chattygit){
		fprintf(stderr, "writepkt: %s:\t", len);
		fwrite(buf, 1, nbuf, stderr);
		for(i = 0; i < nbuf; i++){
			if(isprint(buf[i]))
				fputc(buf[i], stderr);
		}
	}
	return 0;
}


int
got_resolve_remote_ref(struct got_object_id *id, char *ref)
{
	char buf[128], *s;
	int r, f;

	ref = strip(ref);
	if(!got_parse_sha1_digest(id->sha1, ref))
		return 0;

	/* Slightly special handling: translate remote refs to local ones. */
	if (strcmp(ref, "HEAD") == 0) {
		if(snprintf(buf, sizeof(buf), ".git/HEAD") >= sizeof(buf))
			return -1;
	} else if(strstr(ref, "refs/heads") == ref) {
		ref += strlen("refs/heads");
		if(snprintf(buf, sizeof(buf),
		    ".git/refs/remotes/%s/%s", upstream, ref) >= sizeof(buf))
			return -1;
	} else if(strstr(ref, "refs/tags") == ref) {
		ref += strlen("refs/tags");
		if(snprintf(buf, sizeof(buf),
		    ".git/refs/tags/%s/%s", upstream, ref) >= sizeof(buf))
			return -1;
	} else {
		return -1;
	}

	r = -1;
	s = strip(buf);
	if((f = open(s, O_RDONLY)) == -1)
		goto err;
	if(readn(f, buf, sizeof(buf)) < 40)
		goto err;
	if(!got_parse_sha1_digest(id->sha1, buf))
		goto err;
err:
	close(f);
	if(r == -1 && strstr(buf, "ref:") == buf)
		return got_resolve_remote_ref(id, buf + strlen("ref:"));
	return r;
}

static int
got_check_pack_hash(int fd, size_t sz, uint8_t *hcomp)
{
	SHA1_CTX ctx;
	uint8_t hexpect[SHA1_DIGEST_LENGTH];
	char s1[SHA1_DIGEST_STRING_LENGTH + 1];
	char s2[SHA1_DIGEST_STRING_LENGTH + 1];
	uint8_t buf[32*1024];
	ssize_t n, r, nr;

	if(sz < 28)
		return -1;

	n = 0;
	SHA1Init(&ctx);
	while(n < sz - 20){
		nr = sizeof(buf);
		if(sz - n - 20 < sizeof(buf))
			nr = sz - n - 20;
		r = readn(fd, buf, nr);
		if(r != nr)
			return -1;
		SHA1Update(&ctx, buf, nr);
		n += r;
	}
	SHA1Final(hcomp, &ctx);

	if(readn(fd, hexpect, sizeof(hexpect)) != sizeof(hexpect))
		errx(1, "truncated packfile");
	if(memcmp(hcomp, hexpect, SHA1_DIGEST_LENGTH) != 0){
		got_sha1_digest_to_str(hcomp, s1, sizeof(s1));
		got_sha1_digest_to_str(hexpect, s2, sizeof(s2));
		printf("hash mismatch %s != %s\n", s1, s2);
		return -1;
	}
	return 0;
}

int
got_has_object(struct got_object_id *obj)
{
	return 0;
}

/*static */int
got_make_pack_dir(char *path)
{
	char s[128];
	char *p;

	if(snprintf(s, sizeof(s), "%s", path) >= sizeof(s))
		return -1;
	for(p=strchr(s+1, '/'); p; p=strchr(p+1, '/')){
		*p = 0;
		if (mkdir(s, 0755) == -1)
			if(errno != EEXIST)
				return -1;
		*p = '/';
	}
	return 0;
}

static int
got_match_branch(char *br, char *pat)
{
	char name[128];

	if(strstr(pat, "refs/heads") == pat) {
		if (snprintf(name, sizeof(name), "%s", pat) >= sizeof(name))
			return -1;
	} else if(strstr(pat, "heads")) {
		if (snprintf(name, sizeof(name), "refs/%s", pat) >= sizeof(name))
			return -1;
	} else {
		if (snprintf(name, sizeof(name), "refs/heads/%s", pat) >= sizeof(name))
			return -1;
	}
	return strcmp(br, name) == 0;
}

static int
got_tokenize_refline(char *line, char **sp, size_t nsp)
{
	char *p;
	size_t i, j;

	for (i = 0; i < nsp; i++) {
		while (isspace(*line))
			line++;
		p = line;
		while (*line != '\0' && (!isspace(*line) || i == nsp - 1))
			line++;
		sp[i] = strndup(p, line - p);
	}
	for (j = i; j < nsp; j++)
		sp[j] = NULL;
	return i;
}

static const struct got_error *
fetch_pack(int fd, int packfd, struct got_object_id *packid,
    struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	char buf[GOT_PKTMAX], *sp[3];
	char hashstr[SHA1_DIGEST_STRING_LENGTH];
	struct got_object_id *have, *want;
	int nref, refsz;
	int i, n, req;
	off_t packsz;

	nref = 0;
	refsz = 16;
	have = malloc(refsz * sizeof(have[0]));
	if (have == NULL)
		return got_error_from_errno("malloc");
	want = malloc(refsz * sizeof(want[0]));
	if (want == NULL) {
		err = got_error_from_errno("malloc");
		goto done;
	}
	if (chattygit)
		fprintf(stderr, "starting fetch\n");
	while (1) {
		n = readpkt(fd, buf, sizeof(buf));
		if (n == -1){
			err = got_error_from_errno("readpkt");
			goto done;
		}
		if (n == 0)
			break;
		if (strncmp(buf, "ERR ", 4) == 0) {
			static char msg[1024];
			strlcpy(msg, buf + 4, sizeof(msg));
			err = got_error_msg(GOT_ERR_FETCH_FAILED, msg);
			goto done;
		}
		if (got_tokenize_refline(buf, sp, 3) <= 2) {
			err = got_error(GOT_ERR_NOT_REF);
			goto done;
		}
		if (strstr(sp[1], "^{}"))
			continue;
		if (fetchbranch && !got_match_branch(sp[1], fetchbranch))
			continue;
		if (refsz == nref + 1){
			refsz *= 2;
			have = realloc(have, refsz * sizeof(have[0]));
			if (have == NULL) {
				err = got_error_from_errno("realloc");
				goto done;
			}
			want = realloc(want, refsz * sizeof(want[0]));
			if (want == NULL) {
				err = got_error_from_errno("realloc");
				goto done;
			}
		}
		if (!got_parse_sha1_digest(want[nref].sha1, sp[0])) {
			err = got_error(GOT_ERR_BAD_OBJ_ID_STR);
			goto done;
		}

		if (got_resolve_remote_ref(&have[nref], sp[1]) == -1)
			memset(&have[nref], 0, sizeof(have[nref]));
		err = got_privsep_send_fetch_progress(ibuf, &want[nref], sp[1]);
		if (err)
			goto done;
		if (chattygit)
			fprintf(stderr, "remote %s\n", sp[1]);
		nref++;
	}

	req = 0;
	for (i = 0; i < nref; i++){
		if (got_object_id_cmp(&have[i], &want[i]) == 0)
			continue;
		if (got_has_object(&want[i]))
			continue;
		got_sha1_digest_to_str(want[i].sha1, hashstr, sizeof(hashstr));
		n = snprintf(buf, sizeof(buf), "want %s\n", hashstr);
		if (n >= sizeof(buf)) {
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
		if (writepkt(fd, buf, n) == -1) {
			err = got_error_from_errno("writepkt");
			goto done;
		}
		req = 1;
	}
	flushpkt(fd);
	for (i = 0; i < nref; i++){
		if (got_object_id_cmp(&have[i], &zhash) == 0)
			continue;
		got_sha1_digest_to_str(want[i].sha1, hashstr, sizeof(hashstr));
		n = snprintf(buf, sizeof(buf), "have %s\n", hashstr);
		if (n >= sizeof(buf)) {
			err = got_error(GOT_ERR_NO_SPACE);
			goto done;
		}
		if (writepkt(fd, buf, n + 1) == -1) {
			err = got_error_from_errno("writepkt");
			goto done;
		}
	}
	if (!req){
		fprintf(stderr, "up to date\n");
		flushpkt(fd);
	}
	n = snprintf(buf, sizeof(buf), "done\n");
	if (writepkt(fd, buf, n) == -1) {
		err = got_error_from_errno("writepkt");
		goto done;
	}
	if (!req)
		return 0;

	if ((n = readpkt(fd, buf, sizeof(buf))) == -1) {
		err = got_error_from_errno("readpkt");
		goto done;
	}
	buf[n] = 0;

	if (chattygit)
		fprintf(stderr, "fetching...\n");
	packsz = 0;
	while (1) {
		ssize_t w;
		n = readn(fd, buf, sizeof buf);
		if (n == 0)
			break;
		if (n == -1) {
			err = got_error_from_errno("readn");
			goto done;
		}
		w = write(packfd, buf, n);
		if (w == -1) {
			err = got_error_from_errno("write");
			goto done;
		}
		if (w != n) {
			err = got_error(GOT_ERR_IO);
			goto done;
		}
		packsz += n;
	}
	if (lseek(packfd, 0, SEEK_SET) == -1) {
		err = got_error_from_errno("lseek");
		goto done;
	}
	if (got_check_pack_hash(packfd, packsz, packid->sha1) == -1)
		err = got_error(GOT_ERR_BAD_PACKFILE);
done:
	free(have);
	free(want);
	return err;
}


int
main(int argc, char **argv)
{
	const struct got_error *err = NULL;
	int fetchfd, packfd = -1;
	struct got_object_id packid;
	struct imsgbuf ibuf;
	struct imsg imsg;

	if(getenv("GOT_DEBUG") != NULL){
		fprintf(stderr, "fetch-pack being chatty!\n");
		chattygit = 1;
	}

	imsg_init(&ibuf, GOT_IMSG_FD_CHILD);
	if((err = got_privsep_recv_imsg(&imsg, &ibuf, 0)) != 0) {
		if (err->code == GOT_ERR_PRIVSEP_PIPE)
			err = NULL;
		goto done;
	}
	if (imsg.hdr.type == GOT_IMSG_STOP)
		goto done;
	if (imsg.hdr.type != GOT_IMSG_FETCH_REQUEST) {
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		goto done;
	}
	if (imsg.hdr.len - IMSG_HEADER_SIZE != 0) {
		err = got_error(GOT_ERR_PRIVSEP_LEN);
		goto done;
	}
	fetchfd = imsg.fd;

	if((err = got_privsep_recv_imsg(&imsg, &ibuf, 0)) != 0) {
		if (err->code == GOT_ERR_PRIVSEP_PIPE)
			err = NULL;
		goto done;
	}
	if (imsg.hdr.type == GOT_IMSG_STOP)
		goto done;
	if (imsg.hdr.type != GOT_IMSG_TMPFD) {
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		goto done;
	}
	if (imsg.hdr.len - IMSG_HEADER_SIZE != 0) {
		err = got_error(GOT_ERR_PRIVSEP_LEN);
		goto done;
	}
	packfd = imsg.fd;

	err = fetch_pack(fetchfd, packfd, &packid, &ibuf);
	if (err)
		goto done;
done:
	if (packfd != -1 && close(packfd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (err != NULL)
		got_privsep_send_error(&ibuf, err);
	else
		err = got_privsep_send_fetch_done(&ibuf, packid);
	if(err != NULL) {
		fprintf(stderr, "%s: %s\n", getprogname(), err->msg);
		got_privsep_send_error(&ibuf, err);
	}

	exit(0);
}