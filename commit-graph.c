#include "cache.h"
#include "config.h"
#include "git-compat-util.h"
#include "lockfile.h"
#include "pack.h"
#include "packfile.h"
#include "commit.h"
#include "object.h"
#include "revision.h"
#include "sha1-lookup.h"
#include "commit-graph.h"

#define GRAPH_SIGNATURE 0x43475048 /* "CGPH" */
#define GRAPH_CHUNKID_OIDFANOUT 0x4f494446 /* "OIDF" */
#define GRAPH_CHUNKID_OIDLOOKUP 0x4f49444c /* "OIDL" */
#define GRAPH_CHUNKID_DATA 0x43444154 /* "CDAT" */
#define GRAPH_CHUNKID_LARGEEDGES 0x45444745 /* "EDGE" */

#define GRAPH_DATA_WIDTH 36

#define GRAPH_VERSION_1 0x1
#define GRAPH_VERSION GRAPH_VERSION_1

#define GRAPH_OID_VERSION_SHA1 1
#define GRAPH_OID_LEN_SHA1 GIT_SHA1_RAWSZ
#define GRAPH_OID_VERSION GRAPH_OID_VERSION_SHA1
#define GRAPH_OID_LEN GRAPH_OID_LEN_SHA1

#define GRAPH_OCTOPUS_EDGES_NEEDED 0x80000000
#define GRAPH_PARENT_MISSING 0x7fffffff
#define GRAPH_EDGE_LAST_MASK 0x7fffffff
#define GRAPH_PARENT_NONE 0x70000000

#define GRAPH_LAST_EDGE 0x80000000

#define GRAPH_FANOUT_SIZE (4 * 256)
#define GRAPH_CHUNKLOOKUP_WIDTH 12
#define GRAPH_MIN_SIZE (5 * GRAPH_CHUNKLOOKUP_WIDTH + GRAPH_FANOUT_SIZE + \
			GRAPH_OID_LEN + 8)


static char *get_commit_graph_filename(const char *obj_dir)
{
	return xstrfmt("%s/info/commit-graph", obj_dir);
}

static void write_graph_chunk_fanout(struct hashfile *f,
				     struct commit **commits,
				     int nr_commits)
{
	int i, count = 0;
	struct commit **list = commits;

	/*
	 * Write the first-level table (the list is sorted,
	 * but we use a 256-entry lookup to be able to avoid
	 * having to do eight extra binary search iterations).
	 */
	for (i = 0; i < 256; i++) {
		while (count < nr_commits) {
			if ((*list)->object.oid.hash[0] != i)
				break;
			count++;
			list++;
		}

		hashwrite_be32(f, count);
	}
}

static void write_graph_chunk_oids(struct hashfile *f, int hash_len,
				   struct commit **commits, int nr_commits)
{
	struct commit **list = commits;
	int count;
	for (count = 0; count < nr_commits; count++, list++)
		hashwrite(f, (*list)->object.oid.hash, (int)hash_len);
}

static const unsigned char *commit_to_sha1(size_t index, void *table)
{
	struct commit **commits = table;
	return commits[index]->object.oid.hash;
}

static void write_graph_chunk_data(struct hashfile *f, int hash_len,
				   struct commit **commits, int nr_commits)
{
	struct commit **list = commits;
	struct commit **last = commits + nr_commits;
	uint32_t num_extra_edges = 0;

	while (list < last) {
		struct commit_list *parent;
		int edge_value;
		uint32_t packedDate[2];

		parse_commit(*list);
		hashwrite(f, (*list)->tree->object.oid.hash, hash_len);

		parent = (*list)->parents;

		if (!parent)
			edge_value = GRAPH_PARENT_NONE;
		else {
			edge_value = sha1_pos(parent->item->object.oid.hash,
					      commits,
					      nr_commits,
					      commit_to_sha1);

			if (edge_value < 0)
				edge_value = GRAPH_PARENT_MISSING;
		}

		hashwrite_be32(f, edge_value);

		if (parent)
			parent = parent->next;

		if (!parent)
			edge_value = GRAPH_PARENT_NONE;
		else if (parent->next)
			edge_value = GRAPH_OCTOPUS_EDGES_NEEDED | num_extra_edges;
		else {
			edge_value = sha1_pos(parent->item->object.oid.hash,
					      commits,
					      nr_commits,
					      commit_to_sha1);
			if (edge_value < 0)
				edge_value = GRAPH_PARENT_MISSING;
		}

		hashwrite_be32(f, edge_value);

		if (edge_value & GRAPH_OCTOPUS_EDGES_NEEDED) {
			do {
				num_extra_edges++;
				parent = parent->next;
			} while (parent);
		}

		if (sizeof((*list)->date) > 4)
			packedDate[0] = htonl(((*list)->date >> 32) & 0x3);
		else
			packedDate[0] = 0;

		packedDate[1] = htonl((*list)->date);
		hashwrite(f, packedDate, 8);

		list++;
	}
}

static void write_graph_chunk_large_edges(struct hashfile *f,
					  struct commit **commits,
					  int nr_commits)
{
	struct commit **list = commits;
	struct commit **last = commits + nr_commits;
	struct commit_list *parent;

	while (list < last) {
		int num_parents = 0;
		for (parent = (*list)->parents; num_parents < 3 && parent;
		     parent = parent->next)
			num_parents++;

		if (num_parents <= 2) {
			list++;
			continue;
		}

		/* Since num_parents > 2, this initializer is safe. */
		for (parent = (*list)->parents->next; parent; parent = parent->next) {
			int edge_value = sha1_pos(parent->item->object.oid.hash,
						  commits,
						  nr_commits,
						  commit_to_sha1);

			if (edge_value < 0)
				edge_value = GRAPH_PARENT_MISSING;
			else if (!parent->next)
				edge_value |= GRAPH_LAST_EDGE;

			hashwrite_be32(f, edge_value);
		}

		list++;
	}
}

static int commit_compare(const void *_a, const void *_b)
{
	const struct object_id *a = (const struct object_id *)_a;
	const struct object_id *b = (const struct object_id *)_b;
	return oidcmp(a, b);
}

struct packed_commit_list {
	struct commit **list;
	int nr;
	int alloc;
};

struct packed_oid_list {
	struct object_id *list;
	int nr;
	int alloc;
};

static int add_packed_commits(const struct object_id *oid,
			      struct packed_git *pack,
			      uint32_t pos,
			      void *data)
{
	struct packed_oid_list *list = (struct packed_oid_list*)data;
	enum object_type type;
	off_t offset = nth_packed_object_offset(pack, pos);
	struct object_info oi = OBJECT_INFO_INIT;

	oi.typep = &type;
	if (packed_object_info(pack, offset, &oi) < 0)
		die("unable to get type of object %s", oid_to_hex(oid));

	if (type != OBJ_COMMIT)
		return 0;

	ALLOC_GROW(list->list, list->nr + 1, list->alloc);
	oidcpy(&(list->list[list->nr]), oid);
	(list->nr)++;

	return 0;
}

void write_commit_graph(const char *obj_dir)
{
	struct packed_oid_list oids;
	struct packed_commit_list commits;
	struct hashfile *f;
	uint32_t i, count_distinct = 0;
	char *graph_name;
	int fd;
	struct lock_file lk = LOCK_INIT;
	uint32_t chunk_ids[5];
	uint64_t chunk_offsets[5];
	int num_chunks;
	int num_extra_edges;
	struct commit_list *parent;

	oids.nr = 0;
	oids.alloc = approximate_object_count() / 4;

	if (oids.alloc < 1024)
		oids.alloc = 1024;
	ALLOC_ARRAY(oids.list, oids.alloc);

	for_each_packed_object(add_packed_commits, &oids, 0);

	QSORT(oids.list, oids.nr, commit_compare);

	count_distinct = 1;
	for (i = 1; i < oids.nr; i++) {
		if (oidcmp(&oids.list[i-1], &oids.list[i]))
			count_distinct++;
	}

	if (count_distinct >= GRAPH_PARENT_MISSING)
		die(_("the commit graph format cannot write %d commits"), count_distinct);

	commits.nr = 0;
	commits.alloc = count_distinct;
	ALLOC_ARRAY(commits.list, commits.alloc);

	num_extra_edges = 0;
	for (i = 0; i < oids.nr; i++) {
		int num_parents = 0;
		if (i > 0 && !oidcmp(&oids.list[i-1], &oids.list[i]))
			continue;

		commits.list[commits.nr] = lookup_commit(&oids.list[i]);
		parse_commit(commits.list[commits.nr]);

		for (parent = commits.list[commits.nr]->parents;
		     parent; parent = parent->next)
			num_parents++;

		if (num_parents > 2)
			num_extra_edges += num_parents - 1;

		commits.nr++;
	}
	num_chunks = num_extra_edges ? 4 : 3;

	if (commits.nr >= GRAPH_PARENT_MISSING)
		die(_("too many commits to write graph"));

	graph_name = get_commit_graph_filename(obj_dir);
	fd = hold_lock_file_for_update(&lk, graph_name, 0);

	if (fd < 0) {
		struct strbuf folder = STRBUF_INIT;
		strbuf_addstr(&folder, graph_name);
		strbuf_setlen(&folder, strrchr(folder.buf, '/') - folder.buf);

		if (mkdir(folder.buf, 0777) < 0)
			die_errno(_("cannot mkdir %s"), folder.buf);
		strbuf_release(&folder);

		fd = hold_lock_file_for_update(&lk, graph_name, LOCK_DIE_ON_ERROR);

		if (fd < 0)
			die_errno("unable to create '%s'", graph_name);
	}

	f = hashfd(lk.tempfile->fd, lk.tempfile->filename.buf);

	hashwrite_be32(f, GRAPH_SIGNATURE);

	hashwrite_u8(f, GRAPH_VERSION);
	hashwrite_u8(f, GRAPH_OID_VERSION);
	hashwrite_u8(f, num_chunks);
	hashwrite_u8(f, 0); /* unused padding byte */

	chunk_ids[0] = GRAPH_CHUNKID_OIDFANOUT;
	chunk_ids[1] = GRAPH_CHUNKID_OIDLOOKUP;
	chunk_ids[2] = GRAPH_CHUNKID_DATA;
	if (num_extra_edges)
		chunk_ids[3] = GRAPH_CHUNKID_LARGEEDGES;
	else
		chunk_ids[3] = 0;
	chunk_ids[4] = 0;

	chunk_offsets[0] = 8 + (num_chunks + 1) * GRAPH_CHUNKLOOKUP_WIDTH;
	chunk_offsets[1] = chunk_offsets[0] + GRAPH_FANOUT_SIZE;
	chunk_offsets[2] = chunk_offsets[1] + GRAPH_OID_LEN * commits.nr;
	chunk_offsets[3] = chunk_offsets[2] + (GRAPH_OID_LEN + 16) * commits.nr;
	chunk_offsets[4] = chunk_offsets[3] + 4 * num_extra_edges;

	for (i = 0; i <= num_chunks; i++) {
		uint32_t chunk_write[3];

		chunk_write[0] = htonl(chunk_ids[i]);
		chunk_write[1] = htonl(chunk_offsets[i] >> 32);
		chunk_write[2] = htonl(chunk_offsets[i] & 0xffffffff);
		hashwrite(f, chunk_write, 12);
	}

	write_graph_chunk_fanout(f, commits.list, commits.nr);
	write_graph_chunk_oids(f, GRAPH_OID_LEN, commits.list, commits.nr);
	write_graph_chunk_data(f, GRAPH_OID_LEN, commits.list, commits.nr);
	write_graph_chunk_large_edges(f, commits.list, commits.nr);

	finalize_hashfile(f, NULL, CSUM_HASH_IN_STREAM | CSUM_FSYNC);
	commit_lock_file(&lk);

	free(oids.list);
	oids.alloc = 0;
	oids.nr = 0;
}
