#ifndef GRPCTTS_CONF_H
#define GRPCTTS_CONF_H

#define typeof __typeof__
#include <stddef.h>
#include <stdint.h>

#include "grpctts.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <asterisk.h>
#include <asterisk/utils.h>

struct grpctts_buffer_size {
	double fraction;
	double seconds;
};

struct grpctts_job_conf {
	double speed;
	double tone;
	char *model;
	struct grpctts_buffer_size initial_buffer_size;
};

struct grpctts_conf {
	char *endpoint;
	struct grpctts_job_conf job_conf;
};

#define GRPCTTS_JOB_CONF_INITIALIZER {				\
	.speed = 1.0,					\
	.tone = 1.0,						\
	.model = NULL					\
}

#define GRPCTTS_CONF_INITIALIZER {			\
	.endpoint = NULL,				\
	.job_conf = GRPCTTS_JOB_CONF_INITIALIZER,	\
}


extern void grpctts_conf_global_init(void);

extern void grpctts_conf_global_uninit(void);


extern char *grpctts_load_ca_from_file(
	const char *relative_fname);

extern int grpctts_parse_buffer_size(
	struct grpctts_buffer_size *buffer_size,
	const char *str);


extern void grpctts_job_conf_init(
	struct grpctts_job_conf *conf);

extern void grpctts_job_conf_clear(
	struct grpctts_job_conf *conf);

extern struct grpctts_job_conf *grpctts_job_conf_cpy(
	struct grpctts_job_conf *dest,
	const struct grpctts_job_conf *src);


extern void grpctts_conf_init(
	struct grpctts_conf *conf);

extern void grpctts_conf_clear(
	struct grpctts_conf *conf);

extern int grpctts_conf_load(
	struct grpctts_conf *conf,
	ast_mutex_t *mutex,
	const char *fname,
	int reload);

extern struct grpctts_conf *grpctts_conf_cpy(
	struct grpctts_conf *dest,
	const struct grpctts_conf *src,
	ast_mutex_t *src_mutex);

#ifdef __cplusplus
};
#endif
#endif
