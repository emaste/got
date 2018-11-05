/*
 * Copyright (c) 2018 Stefan Sperling <stsp@openbsd.org>
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

#include <sys/queue.h>

#include <stdlib.h>
#include <string.h>
#include <sha1.h>
#include <stdio.h>
#include <zlib.h>
#include <limits.h>
#include <time.h>

#include "got_object.h"
#include "got_error.h"

#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_object_idcache.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

struct got_object_idcache_element {
	TAILQ_ENTRY(got_object_idcache_element) entry;
	struct got_object_id id;
	void *data;	/* API user data */
};

TAILQ_HEAD(got_object_idcache_head, got_object_idcache_element);

struct got_object_idcache {
	/*
	 * The cache is implemented as a collection of 256 lists.
	 * The value of the first byte of an object ID determines
	 * which of these lists an object ID is stored in.
	 */
	struct got_object_idcache_head entries[0xff + 1];
	int nelem[0xff + 1];
	int totelem;
	int maxelem;
};

struct got_object_idcache *
got_object_idcache_alloc(int maxelem)
{
	struct got_object_idcache *cache;
	int i;

	cache = calloc(1, sizeof(*cache));
	if (cache == NULL)
		return NULL;

	for (i = 0; i < nitems(cache->entries); i++)
		TAILQ_INIT(&cache->entries[i]);
	cache->maxelem = maxelem;
	return cache;
}

void
got_object_idcache_free(struct got_object_idcache *cache)
{
	struct got_object_idcache_element *entry;
	int i;

	for (i = 0; i < nitems(cache->entries); i++) {
		while (!TAILQ_EMPTY(&cache->entries[i])) {
			entry = TAILQ_FIRST(&cache->entries[i]);
			TAILQ_REMOVE(&cache->entries[i], entry, entry);
			/* User data should be freed by caller. */
			free(entry);
		}
	}
	free(cache);
}

const struct got_error *
got_object_idcache_add(struct got_object_idcache *cache,
    struct got_object_id *id, void *data)
{
	struct got_object_idcache_element *entry;
	uint8_t i = id->sha1[0];

	if (cache->totelem >= cache->maxelem)
		return got_error(GOT_ERR_NO_SPACE);

	entry = malloc(sizeof(*entry));
	if (entry == NULL)
		return got_error_from_errno();

	memcpy(&entry->id, id, sizeof(entry->id));
	entry->data = data;

	TAILQ_INSERT_HEAD(&cache->entries[i], entry, entry);
	cache->nelem[i]++;
	cache->totelem++;
	return NULL;
}

void *
got_object_idcache_get(struct got_object_idcache *cache, struct got_object_id *id)
{
	struct got_object_idcache_element *entry;
	uint8_t i = id->sha1[0];

	TAILQ_FOREACH(entry, &cache->entries[i], entry) {
		if (memcmp(&entry->id.sha1, id->sha1, SHA1_DIGEST_LENGTH) != 0)
			continue;
		if (entry != TAILQ_FIRST(&cache->entries[i])) {
			TAILQ_REMOVE(&cache->entries[i], entry, entry);
			TAILQ_INSERT_HEAD(&cache->entries[i], entry, entry);
		}
		return entry->data;
	}

	return NULL;
}

const struct got_error *
got_object_idcache_remove_least_used(void **data, struct got_object_idcache *cache)
{
	struct got_object_idcache_element *entry;
	int i, idx = 0, maxelem = cache->nelem[0];

	if (data)
		*data = NULL;

	if (cache->totelem == 0)
		return got_error(GOT_ERR_NO_OBJ);

	/* Remove least used element from longest list. */
	for (i = 0; i < nitems(cache->entries); i++) {
		if (maxelem < cache->nelem[i]) {
			idx = i;
			maxelem = cache->nelem[i];
		}
	}
	entry = TAILQ_LAST(&cache->entries[idx], got_object_idcache_head);
	TAILQ_REMOVE(&cache->entries[idx], entry, entry);
	if (data)
		*data = entry->data;
	free(entry);
	cache->nelem[idx]--;
	cache->totelem--;
	return NULL;
}

int
got_object_idcache_contains(struct got_object_idcache *cache,
    struct got_object_id *id)
{
	struct got_object_idcache_element *entry;
	uint8_t i = id->sha1[0];

	TAILQ_FOREACH(entry, &cache->entries[i], entry) {
		if (memcmp(&entry->id.sha1, id->sha1, SHA1_DIGEST_LENGTH) == 0)
			return 1;
	}

	return 0;
}

void got_object_idcache_for_each(struct got_object_idcache *cache,
    void (*cb)(struct got_object_id *, void *, void *), void *arg)
{
	struct got_object_idcache_element *entry;
	int i;

	for (i = 0; i < nitems(cache->entries); i++) {
		TAILQ_FOREACH(entry, &cache->entries[i], entry)
			cb(&entry->id, entry->data, arg);
	}
}

int
got_object_idcache_num_elements(struct got_object_idcache *cache)
{
	return cache->totelem;
}
