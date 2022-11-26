/*
 * Copyright Tobias Waldekranz <tobias@waldekranz.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <stdio.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>

#include <ply/ply.h>
#include <ply/internal.h>

static int pass_sym_alloc(struct node *n, void *_pb)
{
	struct ply_probe *pb = _pb;
	struct provider *built_in = provider_get("!built-in");
	int err = 0;

	if (n->sym)
		return 0;

	switch (n->ntype) {
	case N_EXPR:
		err = pb->provider->sym_alloc(pb, n);
		if (!err || (err != -ENOENT))
			break;

		/* fall-through */
	case N_NUM:
	case N_STRING:
		err = built_in->sym_alloc(pb, n);
	}

	if (err) {
		if ((err == -ENOENT))
			_ne(n, "unknown symbol '%N'.\n", n);
	} else {
		assert(n->sym);
	}

	return err;
}


static int pass_type_infer(struct node *n, void *_pb)
{
	struct ply_probe *pb = _pb;

	if (n->sym->func->type_infer)
		return n->sym->func->type_infer(n->sym->func, n);

	return 0;
}

static int pass_type_report(struct node *n, void *_pb)
{
	if (!n->sym->type)
		_ne(n, "type of symbol '%N' is unknown\n", n);

	return 0;
}

static int pass_type_validate(struct node *n, void *_pb)
{
	if (!n->sym->type)
		return -EINVAL;

	return 0;
}

static int pass_rewrite(struct node *n, void *_pb)
{
	struct ply_probe *pb = _pb;

	if (n->sym->func->rewrite)
		return n->sym->func->rewrite(n->sym->func, n, pb);

	return 0;
}

static char *pass_ir_comment(struct node *n, const char *phase)
{
	char *comment;

	switch (n->ntype) {
	case N_EXPR:
		asprintf(&comment, "%s %s()", phase, n->expr.func);
		break;
	case N_STRING:
		asprintf(&comment, "%s \"%s\"", phase, n->string.data);
		break;
	case N_NUM:
		if (n->num.unsignd)
			asprintf(&comment, "%s <%#" PRIx64 ">", phase, n->num.u64);
		else
			asprintf(&comment, "%s <%" PRId64 ">", phase, n->num.s64);
		break;
	}

	return comment;
}

static int pass_ir_pre(struct node *n, void *_pb)
{
	struct ply_probe *pb = _pb;
	int err = 0;

	ir_emit_comment(pb->ir, pass_ir_comment(n, ">pre ")); /* TODO comment leaked */

	if (n->sym->func->ir_pre)
		err = n->sym->func->ir_pre(n->sym->func, n, pb);

	/* ir_emit_comment(pb->ir, pass_ir_comment(n, "<pre ")); /\* TODO comment leaked *\/ */
	return err;
}

static int pass_ir_post(struct node *n, void *_pb)
{
	struct ply_probe *pb = _pb;
	int err = 0;

	ir_emit_comment(pb->ir, pass_ir_comment(n, ">post")); /* TODO comment leaked */

	if (n->sym->func->ir_post)
		err = n->sym->func->ir_post(n->sym->func, n, pb);

	/* ir_emit_comment(pb->ir, pass_ir_comment(n, "<post")); /\* TODO comment leaked *\/ */
	return err;
}

static int run_walk(struct ply *ply, nwalk_fn pre, nwalk_fn post)
{
	struct ply_probe *pb;
	int err;

	ply_probe_foreach(ply, pb) {
		err = node_walk(pb->ast, pre, post, pb);
		if (err)
			return err;
	}

	return 0;
}

static int run_ir(struct ply *ply)
{
	struct provider *built_in = provider_get("!built-in");
	struct ply_probe *pb;
	int err;

	ply_probe_foreach(ply, pb) {
		err = pb->provider->ir_pre ?
			pb->provider->ir_pre(pb) : 0;
		if (err)
			return err;

		err = built_in->ir_pre ?
			built_in->ir_pre(pb) : 0;
		if (err)
			return err;

		err = node_walk(pb->ast, pass_ir_pre, pass_ir_post, pb);
		if (err)
			return err;

		err = built_in->ir_post ?
			built_in->ir_post(pb) : 0;
		if (err)
			return err;

		err = pb->provider->ir_post ?
			pb->provider->ir_post(pb) : 0;
		if (err)
			return err;

		ir_emit_insn(pb->ir, EXIT, 0, 0);
	}

	return 0;
}

static int run_bpf(struct ply *ply)
{
	struct ply_probe *pb;
	int err;

	ply_probe_foreach(ply, pb) {
		err = ir_bpf_generate(pb->ir);
		if (err)
			return err;
	}

	return 0;
}

int ply_compile(struct ply *ply)
{
	struct pass *pass;
	int err = 0, rewrites;

	for (rewrites = 0; rewrites < 10; rewrites++) {
		err =         run_walk(ply, NULL, pass_sym_alloc);
		err = err ? : run_walk(ply, NULL, pass_type_infer);
		err = err ? : run_walk(ply, NULL, pass_rewrite);
		if (err < 0)
			return err;

		if (!err)
			break;
	}

	assert(!err);

	err = err ? : run_walk(ply, NULL, pass_sym_alloc);
	err = err ? : run_walk(ply, NULL, pass_type_infer);

	err = err ? : run_walk(ply, NULL, pass_type_report);
	err = err ? : run_walk(ply, NULL, pass_type_validate);

	err = err ? : run_ir(ply);
	err = err ? : run_bpf(ply);

	return err;
}
