/* (C)opyright MMVI Kris Maglione <fbsdaemon at gmail dot com>
 * See LICENSE file for license details.
 */
#include "ixp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void ixp_handle_req(P9Req *r);

/* We use string literals rather than arrays here because
 * they're allocated in a readonly section */
static char
	*Eduptag = "tag in use",
	*Edupfid = "fid in use",
	*Enofunc = "function not implemented",
	*Ebotch = "9P protocol botch",
	*Enofile = "file does not exist",
	*Enofid = "fid does not exist",
	*Enotag = "tag does not exist",
	*Enotdir = "not a directory",
	*Einterrupted = "interrupted",
	*Eisdir = "cannot perform operation on a directory";

enum {	TAG_BUCKETS = 64,
	FID_BUCKETS = 64 };

struct P9Conn {
	Intmap	tagmap;
	void	*taghash[TAG_BUCKETS];
	Intmap	fidmap;
	void	*fidhash[FID_BUCKETS];
	P9Srv	*srv;
	IXPConn	*conn;
	unsigned int	msize;
	unsigned char	*buf;
	unsigned int ref;
};

static void
free_p9conn(P9Conn *pc) {
	free(pc->buf);
	free(pc);
}

static void *
createfid(Intmap *map, int fid, P9Conn *pc) {
	Fid *f = ixp_emallocz(sizeof(Fid));
	f->fid = fid;
	f->omode = -1;
	f->map = map;
	f->conn = pc;
	if(caninsertkey(map, fid, f))
		return f;
	free(f);
	return NULL;
}

static int
destroyfid(P9Conn *pc, unsigned long fid) {
	Fid *f;
	if(!(f = deletekey(&pc->fidmap, fid)))
		return 0;
	if(pc->srv->freefid)
		pc->srv->freefid(f);
	free(f);
	return 1;
}

void
ixp_server_handle_fcall(IXPConn *c) {
	Fcall fcall = {0};
	P9Conn *pc = c->aux;
	P9Req *req;
	unsigned int msize;
	char *errstr = NULL;

	if(!(msize = ixp_recv_message(c->fd, pc->buf, pc->msize, &errstr)))
		goto Fail;
	if(!(msize = ixp_msg2fcall(&fcall, pc->buf, IXP_MAX_MSG)))
		goto Fail;
	req = ixp_emallocz(sizeof(P9Req));
	req->conn = pc;
	req->ifcall = fcall;
	pc->conn = c;
	if(lookupkey(&pc->tagmap, fcall.tag)) {
		respond(req, Eduptag);
		return;
	}
	insertkey(&pc->tagmap, fcall.tag, req);
	ixp_handle_req(req);
	return;
Fail:
	ixp_server_close_conn(c);
}

static void
ixp_handle_req(P9Req *r) {
	P9Conn *pc = r->conn;
	P9Srv *srv = pc->srv;

	switch(r->ifcall.type) {
	default:
		respond(r, Enofunc);
		break;
	case TVERSION:
		if(!strncmp(r->ifcall.version, "9P", 3)) {
			r->ofcall.version = "9P";
		}else
		if(!strncmp(r->ifcall.version, "9P2000", 7)) {
			r->ofcall.version = "9P2000";
		}else{
			r->ofcall.version = "unknown";
		}
		r->ofcall.msize = r->ifcall.msize;
		respond(r, NULL);
		break;
	case TATTACH:
		if(!(r->fid = createfid(&pc->fidmap, r->ifcall.fid, pc))) {
			respond(r, Edupfid);
			return;
		}
		/* attach is a required function */
		srv->attach(r);
		break;
	case TCLUNK:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(!srv->clunk) {
			respond(r, NULL);
			return;
		}
		srv->clunk(r);
		break;
	case TFLUSH:
		if(!(r->oldreq = lookupkey(&pc->tagmap, r->ifcall.oldtag))) {
			respond(r, Enotag);
			return;
		}
		if(!srv->flush) {
			respond(r, Enofunc);
			return;
		}
		srv->flush(r);
		break;
	case TCREATE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(r->fid->omode != -1) {
			respond(r, Ebotch);
			return;
		}
		if(!(r->fid->qid.type&P9QTDIR)) {
			respond(r, Enotdir);
			return;
		}
		if(!pc->srv->create) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->create(r);
		break;
	case TOPEN:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if((r->fid->qid.type&P9QTDIR) && (r->ifcall.mode|P9ORCLOSE) != (P9OREAD|P9ORCLOSE)) {
			respond(r, Eisdir);
			return;
		}
		r->ofcall.qid = r->fid->qid;
		if(!pc->srv->open) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->open(r);
		break;
	case TREAD:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(r->fid->omode == -1 || r->fid->omode == P9OWRITE) {
			respond(r, Ebotch);
			return;
		}
		if(!pc->srv->read) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->read(r);
		break;
	case TREMOVE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(!pc->srv->remove) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->remove(r);
		break;
	case TSTAT:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(!pc->srv->stat) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->stat(r);
		break;
	case TWALK:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if(r->fid->omode != -1) {
			respond(r, "cannot walk from an open fid");
			return;
		}
		if(r->ifcall.nwname && !(r->fid->qid.type&P9QTDIR)) {
			respond(r, Enotdir);
			return;
		}
		if((r->ifcall.fid != r->ifcall.newfid)) {
			if(!(r->newfid = createfid(&pc->fidmap, r->ifcall.newfid, pc))) {
				respond(r, Edupfid);
				return;
			}
		}else
			r->newfid = r->fid;
		if(!pc->srv->walk) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->walk(r);
		break;
	case TWRITE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid))) {
			respond(r, Enofid);
			return;
		}
		if((r->fid->omode&3) != P9OWRITE && (r->fid->omode&3) != P9ORDWR) {
			respond(r, "write on fid not opened for writing");
			return;
		}
		if(!pc->srv->write) {
			respond(r, Enofunc);
			return;
		}
		pc->srv->write(r);
		break;
	/* Still to be implemented: flush, wstat, auth */
	}
}

void
respond(P9Req *r, char *error) {
	P9Conn *pc = r->conn;
	switch(r->ifcall.type) {
	default:
		if(!error)
			assert(!"Respond called on unsupported fcall type");
		break;
	case TVERSION:
		assert(!error);
		free(r->ifcall.version);
		pc->msize = (r->ofcall.msize < IXP_MAX_MSG) ? r->ofcall.msize : IXP_MAX_MSG;
		free(pc->buf);
		pc->buf = ixp_emallocz(r->ofcall.msize);
		break;
	case TATTACH:
		if(error)
			destroyfid(pc, r->fid->fid);
		free(r->ifcall.uname);
		free(r->ifcall.aname);
		break;
	case TOPEN:
	case TCREATE:
		if(!error) {
			r->fid->omode = r->ifcall.mode;
			r->fid->qid = r->ofcall.qid;
		}
		free(r->ifcall.name);
		r->ofcall.iounit = pc->msize - sizeof(unsigned long);
		break;
	case TWALK:
		if(error || r->ofcall.nwqid < r->ifcall.nwname) {
			if(r->ifcall.fid != r->ifcall.newfid && r->newfid)
				destroyfid(pc, r->newfid->fid);
			if(!error && r->ofcall.nwqid == 0)
				error = Enofile;
		}else{
			if(r->ofcall.nwqid == 0)
				r->newfid->qid = r->fid->qid;
			else
				r->newfid->qid = r->ofcall.wqid[r->ofcall.nwqid-1];
		}
		free(*r->ifcall.wname);
		break;
	case TWRITE:
		free(r->ifcall.data);
		break;
	case TREMOVE:
		if(r->fid)
			destroyfid(pc, r->fid->fid);
		break;
	case TCLUNK:
		if(r->fid)
			destroyfid(pc, r->fid->fid);
		if(!pc->conn && r->ifcall.tag == IXP_NOTAG)
			pc->ref--;
		break;
	case TFLUSH:
		if((r->oldreq = lookupkey(&pc->tagmap, r->ifcall.oldtag)))
			respond(r->oldreq, Einterrupted);
		if(!pc->conn && r->ifcall.tag == IXP_NOTAG)
			pc->ref--;
		break;
	case TREAD:
	case TSTAT:
		break;
	/* Still to be implemented: flush, wstat, auth */
	}
	r->ofcall.tag = r->ifcall.tag;
	if(!error)
		r->ofcall.type = r->ifcall.type + 1;
	else {
		r->ofcall.type = RERROR;
		r->ofcall.ename = error;
	}
	if(pc->conn)
		ixp_server_respond_fcall(pc->conn, &r->ofcall);
	switch(r->ofcall.type) {
	case RSTAT:
		free(r->ofcall.stat);
		break;
	case RREAD:
		free(r->ofcall.data);
		break;
	}
	deletekey(&pc->tagmap, r->ifcall.tag);;
	free(r);
	if(!pc->conn && pc->ref == 0)
		free_p9conn(pc);
}

/* Pending request cleanup */
static void
ixp_void_request(void *t) {
	P9Req *r, *tr;
	P9Conn *pc;

	r = t;
	pc = r->conn;
	tr = ixp_emallocz(sizeof(P9Req));
	tr->conn = pc;
	tr->ifcall.type = TFLUSH;
	tr->ifcall.tag = IXP_NOTAG;
	tr->ifcall.oldtag = r->ifcall.tag;
	ixp_handle_req(tr);
}

/* Open FID cleanup */
static void
ixp_void_fid(void *t) {
	P9Conn *pc;
	P9Req *tr;
	Fid *f;

	f = t;
	pc = f->conn;
	tr = ixp_emallocz(sizeof(P9Req));
	tr->fid = f;
	tr->conn = pc;
	tr->ifcall.type = TCLUNK;
	tr->ifcall.tag = IXP_NOTAG;
	tr->ifcall.fid = f->fid;
	ixp_handle_req(tr);
}

static void
ixp_p9conn_incref(void *r) {
	P9Conn *pc = *(P9Conn **)r;
	pc->ref++;
}

/* To cleanup a connction, we increase the ref count for
 * each open FID and pending request and generate clunk and
 * flush requests. As each request is responded to and each
 * FID is clunked, we decrease the ref count. When the ref
 * count is 0, we free the P9Conn and its buf. The IXPConn
 * is taken care of in server.c */
static void
ixp_cleanup_conn(IXPConn *c) {
	P9Conn *pc = c->aux;
	pc->conn = NULL;
	pc->ref = 1;
	execmap(&pc->tagmap, ixp_p9conn_incref);
	execmap(&pc->fidmap, ixp_p9conn_incref);
	if(pc->ref > 1) {
		execmap(&pc->tagmap, ixp_void_request);
		execmap(&pc->fidmap, ixp_void_fid);
	}
	if(--pc->ref == 0)
		free_p9conn(pc);
}

/* Handle incoming 9P connections */
void
serve_9pcon(IXPConn *c) {
	int fd = accept(c->fd, NULL, NULL);
	if(fd < 0)
		return;

	P9Conn *pc = ixp_emallocz(sizeof(P9Conn));
	pc->srv = c->aux;
	/* XXX */
	pc->msize = 1024;
	pc->buf = ixp_emallocz(pc->msize);
	initmap(&pc->tagmap, TAG_BUCKETS, &pc->taghash);
	initmap(&pc->fidmap, FID_BUCKETS, &pc->fidhash);
	ixp_server_open_conn(c->srv, fd, pc, ixp_server_handle_fcall, ixp_cleanup_conn);
}
