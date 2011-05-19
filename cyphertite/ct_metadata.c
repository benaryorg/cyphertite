/* $cyphertite$ */
/*
 * Copyright (c) 2011 Conformal Systems LLC <info@conformal.com>
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

#include <inttypes.h>

#include <assl.h>
#include <clog.h>
#include <exude.h>
#include <xmlsd.h>

#include <ctutil.h>
#include <ct_xml.h>

#include "ct.h"

void ct_md_fileio(void *);
void md_extract_chunk(void *);

const char			*md_filename;
int				md_backup_fd;
int				md_block_no = 0;
size_t				md_size, md_offset;
time_t				md_mtime;

#define MD_O_READ	0
#define MD_O_WRITE	1
void ct_xml_file_open(const char *mfile, int);
void ct_xml_file_close(void);

struct xmlsd_v_elements ct_xml_cmds[] = {
	{ "ct_md_list", xe_ct_md_list },
	{ "ct_md_open_read", xe_ct_md_open_read },
	{ "ct_md_open_create", xe_ct_md_open_create },
	{ "ct_md_delete", xe_ct_md_delete },
	{ NULL, NULL }
};

struct flist			*md_node;
int
ct_md_archive(const char *mfile, char **pat)
{
	struct stat		sb;
	int error, rv;

	/* XXX - pattern is ignored */
	CDBG("opening md file for archive %s", mfile);

	md_backup_fd = open(mfile, O_RDONLY);

	md_offset = 0;
	if (md_backup_fd == -1) {
		CWARNX("unable to open file %s", mfile);
		return -1;
	}
	md_filename = mfile;
	md_node = e_malloc(sizeof(*md_node), E_MEM_CLEAR);
	CDBG("mdnode %p", md_node);

	error = fstat(md_backup_fd, &sb);
	if (error) {
		CWARNX("file stat error %s %d %s",
		    mfile, errno, strerror(errno));
		return -1;
	} else {
		md_size = sb.st_size;
		md_mtime = sb.st_mtime;
	}

	ct_event_init();

	ct_setup_assl();

	/* setup wakeup channels */
	ct_setup_wakeup_file(ct_state, ct_md_fileio);
	ct_setup_wakeup_sha(ct_state, ct_compute_sha);
	ct_setup_wakeup_compress(ct_state, ct_compute_compress);
	ct_setup_wakeup_csha(ct_state, ct_compute_csha);
	ct_setup_wakeup_encrypt(ct_state, ct_compute_encrypt);
	ct_setup_wakeup_complete(ct_state, ct_process_complete);


	ct_xml_file_open(mfile, MD_O_WRITE);

	/* poke file into action */
	ct_wakeup_file();

	/* start turning crank */
	rv = ct_event_dispatch();
	if (rv != 0)
		CWARNX("event_dispatch returned, %d %s", errno,
		    strerror(errno));
	return (rv);
}

void
ct_md_fileio(void *unused)
{
	struct stat		sb;
	size_t                  rsz, rlen;
	struct ct_trans		*ct_trans;
	int			error;

	CDBG("md_fileio entered for block %d", md_block_no);
loop:
	ct_trans = ct_trans_alloc();
	if (ct_trans == NULL) {
		/* system busy, return */
		CDBG("ran out of transactions, waiting");
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	/* perform read */
	rsz = md_size - md_offset;

	if (rsz == 0) {
		ct_set_file_state(CT_S_FINISHED);
		ct_trans->tr_fl_node = NULL;
		ct_trans->tr_state = TR_S_DONE;
		ct_trans->tr_eof = 1;
		ct_trans->tr_trans_id = ct_trans_id++;
		ct_trans->hdr.c_flags = C_HDR_F_METADATA;
		ct_queue_transfer(ct_trans);
		return;
	}
	CDBG("rsz %zd max %d", rsz, ct_max_block_size);
	if (rsz > ct_max_block_size) {
		rsz = ct_max_block_size;
	}

	ct_trans->tr_dataslot = 0;
	rlen = read(md_backup_fd, ct_trans->tr_data[0], rsz);

	CDBG("read %zd", rlen);

	ct_stats->st_bytes_read += rlen;

	ct_trans->tr_fl_node = md_node;
	ct_trans->tr_size[0] = rlen;
	ct_trans->tr_state = TR_S_READ;
	ct_trans->tr_type = TR_T_WRITE_CHUNK;
	ct_trans->tr_trans_id = ct_trans_id++;
	ct_trans->tr_eof = 0;
	ct_trans->hdr.c_flags = C_HDR_F_METADATA;

	CDBG(" trans %"PRId64", read size %zd, into %p rlen %zd",
	    ct_trans->tr_trans_id, rsz, ct_trans->tr_data[0], rlen);

	/*
	 * init iv to something that can be recreated, used if hdr->c_flags
	 * has C_HDR_F_METADATA set.
	 */
	bzero(ct_trans->tr_iv, sizeof(ct_trans->tr_iv));
	ct_trans->tr_iv[0] = (md_block_no >>  0) & 0xff;
	ct_trans->tr_iv[1] = (md_block_no >>  8) & 0xff;
	ct_trans->tr_iv[2] = (md_block_no >> 16) & 0xff;
	ct_trans->tr_iv[3] = (md_block_no >> 24) & 0xff;
	ct_trans->tr_iv[4] = (md_block_no >>  0) & 0xff;
	ct_trans->tr_iv[5] = (md_block_no >>  8) & 0xff;
	ct_trans->tr_iv[6] = (md_block_no >> 16) & 0xff;
	ct_trans->tr_iv[7] = (md_block_no >> 24) & 0xff;
	/* XXX - leaves the rest of the iv with 0 */

	md_block_no++;

	CDBG("sizes rlen %zd offset %zd size %zd", rlen, md_offset, md_size);

	if (rsz != rlen || rlen == 0 ||
	    ((rlen + md_offset) == md_size)) {
		CDBG("DONE");
		/* short read, file truncated, or end of file */
		/* restat file for modifications */
		error = fstat(md_backup_fd, &sb);
		if (error) {
			CWARNX("file stat error %s %d %s",
			    md_filename, errno, strerror(errno));
		} else if (sb.st_size != md_size) {
			CWARNX("file truncated during backup %s",
			    md_filename);
			/*
			 * may need to perform special nop processing
			 * to pad archive file to right number of chunks
			 */
		}
		CDBG("done with md sending");
		CDBG("setting eof on trans %" PRIu64, ct_trans->tr_trans_id);
		close(md_backup_fd);
		md_backup_fd = -1;
		ct_trans->tr_eof = 1;
		ct_queue_transfer(ct_trans);

		md_offset = md_size;
	} else {
		md_offset += rlen;
	}
	if (md_backup_fd != -1) {
		/* if -1, is sent above */
		ct_queue_transfer(ct_trans);
		goto loop;
	}
}

void
ct_xml_file_open(const char *file, int mode)
{
	struct ct_header	*hdr = NULL;
	struct ct_trans		*trans;
	char			*xml_ptr = NULL;
	char			*xml_head = NULL;
	char			*version, *test;
	int			xml_len = 0, slen, rem;

	version = "1.0";
	test	= "false";

	trans = ct_trans_alloc();

	CDBG("setting up XML");
for_real:
	xml_ptr = xml_head;
	slen = 0;

	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_ptr ? xml_len - (xml_ptr - xml_head) : 0;

	CDBG("setting up XML header");
	slen += snprintf(xml_ptr, rem,
            "<?xml version=\"1.0\"?>\r\n"
            "<cr_md_open_%s version=\"%s\" test=\"%s\">\r\n",
	    (mode ? "create" : "read"),
	    version, test);

	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_head ? xml_len - (xml_ptr - xml_head) : 0;
	slen += snprintf(xml_ptr, rem,
	    "<file name=\"%s\"/>\r\n", file);

	CDBG("setting up XML tail");
	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_head ? xml_len - (xml_ptr - xml_head) : 0;

	slen += snprintf(xml_ptr, rem,
	    "</cr_md_open_%s>\r\n\r\n", mode ? "create" : "read");

	if (xml_head == NULL) {
		CDBG("setting up XML body");
		xml_len = slen;
		hdr = &trans->hdr;
		hdr->c_version = C_HDR_VERSION;
		hdr->c_opcode = C_HDR_O_XML;
		hdr->c_size = xml_len+1;

		/*
		 * XXX - yes I think this should be seperate
		 * so that xml size is independant of chunk size
		 */
		xml_head = (char *)ct_body_alloc(NULL, hdr);
		CDBG("got body %p", xml_head);
		goto for_real;
	}

	CDBG("open trans %"PRIu64, trans->tr_trans_id);

	CDBG("setting up XML done with body %s", xml_head);
	if (xml_len != slen) {
		CFATALX("xml list transaction len did not match %d %d",
		    xml_len, slen);
	}

	ct_assl_write_op(ct_assl_ctx, hdr, xml_head);
}

void
ct_xml_file_close(void)
{
	struct ct_header	*hdr = NULL;
	struct ct_trans		*trans;
	char			*xml_ptr = NULL;
	char			*xml_head = NULL;
	char			*version, *test;
	int			xml_len = 0, slen, rem;

	version = "1.0";
	test	= "false";

	trans = ct_trans_alloc();

	CDBG("setting up XML");
for_real:
	xml_ptr = xml_head;
	slen = 0;

	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_ptr ? xml_len - (xml_ptr - xml_head) : 0;

	CDBG("setting up XML header");
	slen += snprintf(xml_ptr, rem,
            "<?xml version=\"1.0\"?>\r\n"
            "<cr_md_close version=\"%s\" test=\"%s\">\r\n",
	    version, test);

	CDBG("setting up XML tail");
	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_head ? xml_len - (xml_ptr - xml_head) : 0;

	slen += snprintf(xml_ptr, rem,
	    "</cr_md_close>\r\n\r\n");

	if (xml_head == NULL) {
		CDBG("setting up XML body");
		xml_len = slen;
		hdr = &trans->hdr;
		hdr->c_version = C_HDR_VERSION;
		hdr->c_opcode = C_HDR_O_XML;
		hdr->c_size = xml_len+1;

		/*
		 * XXX - yes I think this should be seperate
		 * so that xml size is independant of chunk size
		 */
		xml_head = (char *)ct_body_alloc(NULL, hdr);
		CDBG("got body %p", xml_head);
		goto for_real;
	}

	CDBG("setting up XML done with body %s", xml_head);
	if (xml_len != slen) {
		CFATALX("xml list transaction len did not match %d %d",
		    xml_len, slen);
	}

	ct_assl_write_op(ct_assl_ctx, hdr, xml_head);
}

int
ct_md_extract(const char *mfile, char **pat)
{
	int rv;

	CDBG("yo");
	ct_event_init();

	ct_setup_assl();

	/* setup wakeup channels */
	/* XXX - bet these are not the right functions for md_extract */
	ct_setup_wakeup_file(ct_state, md_extract_chunk);
	ct_setup_wakeup_sha(ct_state, ct_compute_sha);
	ct_setup_wakeup_compress(ct_state, ct_compute_compress);
	ct_setup_wakeup_csha(ct_state, ct_compute_csha);
	ct_setup_wakeup_encrypt(ct_state, ct_compute_encrypt);
	ct_setup_wakeup_complete(ct_state, ct_process_complete);

	 /*XXX -chmod when done */
	md_backup_fd = open(mfile, O_WRONLY|O_TRUNC|O_CREAT, 0600);
	if (md_backup_fd == -1) {
		CWARNX("unable to open file %s", mfile);
		return -1;
	}

	ct_xml_file_open(mfile, MD_O_READ);

	/* poke file into action */
	ct_wakeup_file();

	/* start turning crank */
	rv = ct_event_dispatch();
	if (rv != 0)
		CWARNX("event_dispatch returned, %d %s", errno,
		    strerror(errno));
	return 0;
}
int extract_id;

void
md_extract_chunk(void *unused)
{
	struct ct_trans		*trans;
	struct ct_header	*hdr;
	void			*data;

	trans = ct_trans_alloc();
	if (trans == NULL) {
		/* system busy, return */
		CDBG("ran out of transactions, waiting");
		ct_set_file_state(CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_fl_node = md_node;
	trans->tr_state = TR_S_EX_READ;
	trans->tr_type = TR_T_READ_CHUNK;
	trans->tr_trans_id = ct_trans_id++;
	trans->tr_eof = 0;
	trans->hdr.c_flags = C_HDR_F_METADATA;

	hdr = &trans->hdr;

	hdr->c_opcode = C_HDR_O_READ;
	hdr->c_size = sizeof(trans->tr_sha);
	hdr->c_version = C_HDR_VERSION;
	hdr->c_flags |= C_HDR_F_METADATA;

	bzero(trans->tr_sha, sizeof(trans->tr_sha));
	trans->tr_sha[0] = (extract_id >>  0) & 0xff;
	trans->tr_sha[1] = (extract_id >>  8) & 0xff;
	trans->tr_sha[2] = (extract_id >> 16) & 0xff;
	trans->tr_sha[3] = (extract_id >> 24) & 0xff;
	bzero(trans->tr_iv, sizeof(trans->tr_iv));
	trans->tr_iv[0] = (extract_id >>  0) & 0xff;
	trans->tr_iv[1] = (extract_id >>  8) & 0xff;
	trans->tr_iv[2] = (extract_id >> 16) & 0xff;
	trans->tr_iv[3] = (extract_id >> 24) & 0xff;
	trans->tr_iv[4] = (extract_id >>  0) & 0xff;
	trans->tr_iv[5] = (extract_id >>  8) & 0xff;
	trans->tr_iv[6] = (extract_id >> 16) & 0xff;
	trans->tr_iv[7] = (extract_id >> 24) & 0xff;
	data = trans->tr_sha;
	TAILQ_INSERT_TAIL(&ct_state->ct_queued, trans, tr_next);
	ct_state->ct_queued_qlen++;

	extract_id ++; /* next chunk on next pass */

	ct_assl_write_op(ct_assl_ctx, hdr, data);
}

uint64_t ct_md_packet_id;
void
ct_md_wmd(void *vctx)
{
	struct ct_trans *trans;

	/* this probably is not the right activity for md upload complete... */

	trans = RB_MIN(ct_trans_lookup, &ct_state->ct_complete);

	while (trans != NULL && trans->tr_trans_id == ct_md_packet_id) {
		RB_REMOVE(ct_trans_lookup, &ct_state->ct_complete, trans);
		ct_state->ct_complete_rblen--;

		CDBG("writing md type %d trans %" PRIu64 " eof %d",
		    trans->hdr.c_opcode, trans->tr_trans_id, trans->tr_eof);

		ct_md_packet_id++;
		if (trans->tr_eof == 1) {
			close(md_backup_fd);
			ct_shutdown();
		}

		ct_trans_free(trans);

		trans = RB_MIN(ct_trans_lookup, &ct_state->ct_complete);
	}
	if (trans != NULL && trans->tr_trans_id < ct_md_packet_id) {
		CFATALX("old transaction found in completion queue %" PRIu64 " %" PRIu64,
		    trans->tr_trans_id, ct_md_packet_id);
	}
}

void
ct_md_wfile(void *vctx)
{
	struct ct_trans		*trans;
	ssize_t			wlen;
	int			slot;

	trans = RB_MIN(ct_trans_lookup, &ct_state->ct_complete);

	while (trans != NULL && trans->tr_trans_id == ct_md_packet_id) {
		RB_REMOVE(ct_trans_lookup, &ct_state->ct_complete, trans);
		ct_state->ct_complete_rblen--;

		CDBG("writing file trans %" PRIu64 " eof %d", trans->tr_trans_id,
			trans->tr_eof);

		ct_md_packet_id++;

		switch(trans->tr_state) {
		case TR_S_EX_READ:
		case TR_S_EX_DECRYPTED:
		case TR_S_EX_UNCOMPRESSED:
			if (trans->hdr.c_status == C_HDR_S_OK) {
				slot = trans->tr_dataslot;
				CDBG("writing packet sz %d",
				    trans->tr_size[slot]);
				wlen = write(md_backup_fd, trans->tr_data[slot],
				    trans->tr_size[slot]);
				if (wlen != trans->tr_size[slot])
					CWARN("unable to write to md file");
			} else {
				ct_state->ct_file_state = CT_S_FINISHED;
			}
			break;

		case TR_S_EX_DONE:
			if (ct_verbose_ratios)
				ct_dump_stats();

			ct_file_extract_fixup();
			ct_shutdown();
			break;
#if 0
		case TR_S_XML_OPEN: /* XXX ? */
		case TR_S_XML_CLOSE: /* XXX ? */
#endif
		default:
			CFATALX("unexpected tr state in md_wfile %d",
			    trans->tr_state);
		}
		ct_trans_free(trans);

		if (ct_state->ct_file_state != CT_S_FINISHED)
			ct_wakeup_file();

		trans = RB_MIN(ct_trans_lookup, &ct_state->ct_complete);
	}
	if (trans != NULL && trans->tr_trans_id < ct_md_packet_id) {
		CFATALX("old transaction found in completion queue %" PRIu64 " %" PRIu64,
		    trans->tr_trans_id, ct_md_packet_id);
	}
}

int
ct_md_list(const char *mfile, char **pat)
{
	/* XXX - does mfile make sense for md list */
	struct ct_header	*hdr;
	struct ct_trans		*trans;
	char			*xml_ptr = NULL;
	char			*xml_head = NULL;
	char			*version, *test;
	int			xml_len = 0, slen, rem;
	int			rv;

	ct_event_init();

	ct_setup_assl();

	trans = ct_trans_alloc();
	
	version = "1.0";
	test	= "false";

	CDBG("setting up XML");
for_real:
	xml_ptr = xml_head;
	slen = 0;

	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_ptr ? xml_len - (xml_ptr - xml_head) : 0;

	CDBG("setting up XML header");
	slen += snprintf(xml_ptr, rem,
            "<?xml version=\"1.0\"?>\r\n"
            "<cr_md_list version=\"%s\" test=\"%s\">\r\n",
	    version, test);

	/* XXX - pat */
#if 0
	type = "regex";
	type = "glob";
	/* XXX !!!! must be able to go thru this twice!!! */
	for (... pat ...) {
		xml_ptr = xml_head ? (xml_head + slen) : NULL;
		rem = xml_head ? xml_len - (xml_ptr - xml_head) : 0;

		slen += snprintf(xml_ptr, rem,
		    "<%s>%s</%s>\r\n",
		    type, pat[n], type);
	}
#endif

	CDBG("setting up XML tail");
	xml_ptr = xml_head ? (xml_head + slen) : NULL;
	rem = xml_head ? xml_len - (xml_ptr - xml_head) : 0;

	slen += snprintf(xml_ptr, rem,
	    "</cr_md_list>\r\n");

	if (xml_head == NULL) {
		CDBG("setting up XML body");
		xml_len = slen;
		hdr = &trans->hdr;
		hdr->c_version = C_HDR_VERSION;
		hdr->c_opcode = C_HDR_O_XML;
		hdr->c_size = xml_len+1;

		/*
		 * XXX - yes I think this should be seperate
		 * so that xml size is independant of chunk size
		 */
		xml_head = (char *)ct_body_alloc(NULL, hdr);
		CDBG("got body %p", xml_head);
		goto for_real;
	}
	
	CDBG("setting up XML done with body %s", xml_head);
	if (xml_len != slen) {
		CFATALX("xml list transaction len did not match %d %d",
		    xml_len, slen);
	}

	ct_assl_write_op(ct_assl_ctx, hdr, xml_head);

	/* start turning crank */
	rv = ct_event_dispatch();
	if (rv != 0)
		CWARNX("event_dispatch returned, %d %s", errno,
		    strerror(errno));
	return rv;
}

int
ct_md_delete(const char *md, char **arg)
{
	static const char *ct_delete_md = 
            "<?xml version=\"1.0\"?>\r\n"
            "<cr_md_delete version=\"1.0\" test=\"no\">\r\n"
	    "<file name=\"%s\"/>\r\n"
	    "</cr_md_delete>\r\n";
	struct ct_header *hdr;
	struct ct_trans *trans;
	char *cmd, *body = NULL;
	int rv;

	CDBG("ct_md_delete");

	asprintf(&cmd, ct_delete_md, md);

	ct_event_init();
	ct_setup_assl();

	trans = ct_trans_alloc();
	hdr = &trans->hdr;
	hdr->c_version = C_HDR_VERSION;
	hdr->c_opcode = C_HDR_O_XML;
	hdr->c_size = strlen(cmd) + 1;

	body = ct_body_alloc(NULL, hdr);
	bcopy(cmd, body, strlen(cmd)+1);
	free(cmd);

	ct_assl_write_op(ct_assl_ctx, hdr, body);

	/* start turning crank */
	rv = ct_event_dispatch();
	if (rv != 0)
		CWARNX("event_dispatch returned, %d %s", errno,
		    strerror(errno));

	return rv;
}

void
ct_handle_xml_reply(struct ct_trans *trans, struct ct_header *hdr,
    void *vbody)
{
	struct xmlsd_element_list xl;
	struct xmlsd_attribute *xa;
	struct xmlsd_element *xe;
	char *body = vbody;
	char *filename;
	int r;

	CDBG("xml [%s]", (char *)vbody);

	/* Dispose of last parsed command. */
	TAILQ_INIT(&xl);

	r = xmlsd_parse_mem(body, hdr->c_size - 1, &xl);
	if (r) {
		CDBG("parse FAILED! (%d)", r);
		goto done;
	}

	TAILQ_FOREACH(xe, &xl, entry) {
		CDBG("%d %s = %s (parent = %s)",
		    xe->depth, xe->name, xe->value ? xe->value : "NOVAL",
		    xe->parent ? xe->parent->name : "NOPARENT");
		TAILQ_FOREACH(xa, &xe->attr_list, entry)
			CDBG("\t%s = %s", xa->name, xa->value);
	}

	r = xmlsd_validate(&xl, ct_xml_cmds);
	if (r) {
		CDBG("validate of '%s' FAILED! (%d)", body, r);
		goto done;
	}

	if (TAILQ_EMPTY(&xl)) {
		CDBG("parse command: No XML");
		goto done;
	}

	xe = TAILQ_FIRST(&xl);
	if (strcmp(xe->name, "cr_md_list") == 0) {
		TAILQ_FOREACH(xe, &xl, entry) {
			if (strcmp(xe->name, "file") == 0) {
				filename =xmlsd_get_attr(xe, "name");
				if (filename)
					printf("%s uploaded\n", filename);
			}
		}
		ct_shutdown();
	}
	if (strcmp(xe->name, "cr_md_delete") == 0) {
		TAILQ_FOREACH(xe, &xl, entry) {
			if (strcmp(xe->name, "file") == 0) {
				filename =xmlsd_get_attr(xe, "name");
				if (filename)
					printf("%s deleted\n", filename);
			}
		}
		ct_shutdown();
	}

done:
	xmlsd_unwind(&xl);
}
