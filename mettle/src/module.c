#include <stdlib.h>
#include <string.h>
#include <ftw.h>

#include "json.h"
#include "log.h"
#include "module.h"
#include "process.h"
#include "uthash.h"

struct module
{
	struct modulemgr *mm;
	char *path, *fullname;
	const char *name, *description, *date, *license, *rank;
	UT_hash_handle hh;
};

struct modulemgr
{
	struct module *modules;
	struct {
		void (*line)(const char *fmt, ...);
		void (*info)(const char *fmt, ...);
		void (*good)(const char *fmt, ...);
		void (*bad)(const char *fmt, ...);
	} log;
	struct ev_loop *loop;
	struct procmgr *procmgr;
};

void modulemgr_free(struct modulemgr *mm)
{
	if (mm) {
		if (mm->modules) {
			struct module *module, *tmp;
			HASH_ITER(hh, mm->modules, module, tmp) {
				HASH_DEL(mm->modules, module);
				free(module->path);
			}
		}
		if (mm->procmgr) {
			procmgr_free(mm->procmgr);
		}
		free(mm);
	}
}

struct modulemgr * modulemgr_new(struct ev_loop *loop)
{
	struct modulemgr *mm = calloc(1, sizeof(*mm));
	mm->loop = loop;
	mm->procmgr = procmgr_new(loop);
	return mm;
}

void modulemgr_register_log_cbs(struct modulemgr *mm,
	void (*line)(const char *fmt, ...),
	void (*info)(const char *fmt, ...),
	void (*good)(const char *fmt, ...),
	void (*bad)(const char *fmt, ...))
{
	mm->log.line = line;
	mm->log.info = info;
	mm->log.good = good;
	mm->log.bad = bad;
}

struct module * module_new(struct modulemgr *mm, const char *path)
{
	struct module *m = calloc(1, sizeof(*m));
	if (m) {
		m->mm = mm;
		m->path = strdup(path);
		m->fullname = strdup(strstr(path, "modules") + 8);
		char *ext = strchr(m->fullname, '.');
		if (ext) {
			*ext = '\0';
		}
	}
	return m;
}

struct module ** modulemgr_find_modules(struct modulemgr *mm,
	const char *pattern, int *num_modules)
{
	*num_modules = 0;
	struct module *module, *tmp;
	struct module **results = NULL;
	HASH_ITER(hh, mm->modules, module, tmp) {
		if (strncmp(pattern, module->fullname, strlen(pattern)) == 0) {
			results = reallocarray(results, *num_modules + 1, sizeof(struct module *));
			if (results) {
				results[*num_modules] = module;
				(*num_modules)++;
			}
		}
	}
	return results;
}

const char *module_name(struct module *m)
{
	return m->fullname;
}

struct module_ctx {
	struct json_tokener *tok;
	struct json_rpc *jrpc;
	struct module *m;
	struct modulemgr *mm;
};

struct module_ctx * module_ctx_new(struct module *m)
{
	struct module_ctx *ctx = calloc(1, sizeof(*ctx));
	if (ctx) {
		ctx->tok = json_tokener_new();
		ctx->jrpc = json_rpc_new(JSON_RPC_CHECK_VERSION);
		ctx->m = m;
		ctx->mm = m->mm;
	}
	return ctx;
}

void module_ctx_free(struct module_ctx *ctx)
{
	if (ctx) {
		json_tokener_free(ctx->tok);
		json_rpc_free(ctx->jrpc);
		free(ctx);
	}
}

static void module_exit(struct process *p, int exit_status, void *arg)
{
	struct module_ctx *ctx = arg;
	module_ctx_free(ctx);
}

static void module_read_json(struct json_object *obj, void *arg)
{
	struct module_ctx *ctx = arg;
	json_rpc_process(ctx->jrpc, obj);
}

static void module_read_stdout(struct process *p, struct buffer_queue *queue, void *arg)
{
	struct module_ctx *ctx = arg;
	json_read_buffer_queue_cb(queue, ctx->tok, module_read_json, arg);
}

static void module_read_stderr(struct process *p, struct buffer_queue *queue, void *arg)
{
	struct module_ctx *ctx = arg;
	ctx->mm->log.bad("got error from module %s", ctx->m->fullname);
	void *data = NULL;
	ssize_t msg_len = buffer_queue_remove_all(queue, &data);
	if (data) {
		char *line = strtok(data, "\n");
		do {
			ctx->mm->log.bad("%s", line);
		} while ((line = strtok(NULL, "\n")));
	}
	free(data);
}

void module_describe_cb(struct json_result_info *result, void *arg)
{
	struct module_ctx *ctx = arg;
	struct module *m = ctx->m;
	struct json_object *res = result->response;
	json_get_str(res, "name", &m->name);
	json_get_str(res, "description", &m->description);
	json_get_str(res, "date", &m->date);
	json_get_str_def(res, "license", &m->license, "Metasploit Framework License (BSD)");
	json_get_str_def(res, "rank", &m->rank, "Excellent");

	void (*log_line)(const char *fmt, ...) = m->mm->log.line;
	log_line("");
	log_line("       Name: %s", m->name);
	log_line("     Module: %s", m->fullname);
	log_line("    License: %s", m->license);
	log_line("       Rank: %s", m->rank);
	log_line("       Date: %s", m->date);
	log_line("");
}

int module_describe(struct module *m)
{
	struct module_ctx *ctx = module_ctx_new(m);
	struct process_options opts = {.flags = PROCESS_CREATE_SUBSHELL};
	struct process *p = process_create_from_executable(
		ctx->mm->procmgr, ctx->m->path, &opts);
	process_set_callbacks(p, module_read_stdout, module_read_stderr, module_exit, ctx);

	int64_t id;
	struct json_object *call = json_rpc_gen_method_call(ctx->jrpc, "describe", &id, NULL);
	json_rpc_register_result_cb(ctx->jrpc, id, module_describe_cb, ctx);
	const char *msg = json_object_to_json_string_ext(call, 0);
	process_write(p, msg, strlen(msg));

	return 0;
}

void module_log_info(struct module *m)
{
	module_describe(m);
}

static struct modulemgr *_mm;
static int scan_module_path(const char *path,
	const struct stat *s, int flag, struct FTW *f)
{
	if (flag == FTW_F && s->st_mode & S_IXUSR) {
		struct module *m = module_new(_mm, path);
		HASH_ADD_STR(_mm->modules, fullname, m);
	}
	return 0;
}

int modulemgr_load_path(struct modulemgr *mm, const char *path)
{
	_mm = mm;
	log_info("adding modules from %s\n", path);
	return (nftw(path, scan_module_path, 10, 0));
}

int module_run(struct module *m)
{
	return 0;
}
