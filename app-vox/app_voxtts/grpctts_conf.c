

extern struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#define _GNU_SOURCE 1
#include "grpctts_conf.h"

#include <stdio.h>
#include <regex.h>
#include <sys/stat.h>
#include <asterisk.h>
#include <asterisk/paths.h>
#include <asterisk/pbx.h>


#define MAX_INMEMORY_FILE_SIZE (256*1024*1024)


static regex_t cre_fraction;
static int cre_fraction_status;
static regex_t cre_seconds;
static int cre_seconds_status;

void grpctts_conf_global_init(void)
{
	cre_fraction_status = regcomp(&cre_fraction, "\\([0-9]\\+\\(.[0-9]*\\)\\?\\)\\%", 0);
	cre_seconds_status = regcomp(&cre_seconds, "\\([0-9]\\+\\(.[0-9]*\\)\\?\\)s", 0);
}

void grpctts_conf_global_uninit(void)
{
	if (!cre_fraction_status)
		regfree(&cre_fraction);
	if (!cre_seconds_status)
		regfree(&cre_seconds);
}



static int match_fraction(double *fraction_r, const char *str)
{
	if (cre_fraction_status)
		return 0;

	size_t len = strlen(str);

	regmatch_t pmatch[3];
	if (regexec(&cre_fraction, str, 3, pmatch, 0))
		return 0;
	if (pmatch[0].rm_so || pmatch[0].rm_eo != len)
		return 0;

	if (pmatch[1].rm_eo == -1)
		return 0;

	char buffer[4096];
	snprintf(buffer, sizeof(buffer), "%.*s", (int) (pmatch[1].rm_eo - pmatch[1].rm_so), str + pmatch[1].rm_so);

	char *eptr;
	double fraction = strtod(buffer, &eptr);
	if (eptr != (buffer + strlen(buffer)))
		return 0;

	*fraction_r = fraction;
	return 1;
}

static int match_seconds(double *fraction_r, const char *str)
{
	if (cre_seconds_status)
		return 0;

	size_t len = strlen(str);

	regmatch_t pmatch[3];
	if (regexec(&cre_seconds, str, 3, pmatch, 0))
		return 0;
	if (pmatch[0].rm_so || pmatch[0].rm_eo != len || pmatch[1].rm_eo == -1)
		return 0;

	char buffer[4096];
	snprintf(buffer, sizeof(buffer), "%.*s", (int) (pmatch[1].rm_eo - pmatch[1].rm_so), str + pmatch[1].rm_so);

	char *eptr;
	double fraction = strtod(buffer, &eptr);
	if (eptr != (buffer + strlen(buffer)))
		return 0;

	*fraction_r = fraction;
	return 1;
}

int grpctts_parse_buffer_size(struct grpctts_buffer_size *buffer_size, const char *str)
{
	const char *sep_pos = strchr(str, '+');
	if (sep_pos) {
		char buffer_fraction[4096];
		snprintf(buffer_fraction, sizeof(buffer_fraction), "%.*s", (int) (sep_pos - str), str);
		char buffer_seconds[4096];
		snprintf(buffer_seconds, sizeof(buffer_seconds), "%s", sep_pos + 1);
		return match_fraction(&buffer_size->fraction, buffer_fraction) &&
			match_seconds(&buffer_size->seconds, buffer_seconds);
	} else if (match_fraction(&buffer_size->fraction, str)) {
		buffer_size->seconds = 0.0;
	} else if (match_seconds(&buffer_size->seconds, str)) {
		buffer_size->fraction = 0.0;
	} else {
		return 0;
	}

	return 1;
}



void grpctts_job_conf_init(struct grpctts_job_conf *conf)
{
	conf->speed = 1.0;
	conf->tone = 1.0;
	conf->model = NULL;
	conf->initial_buffer_size.fraction = 0.0;
	conf->initial_buffer_size.seconds = 0.0;
}

void grpctts_job_conf_clear(struct grpctts_job_conf *conf)
{
	ast_free(conf->model);
	conf->speed = 1.0;
	conf->tone = 1.0;
	conf->model = NULL;
	conf->initial_buffer_size.fraction = 0.0;
	conf->initial_buffer_size.seconds = 0.0;
}
struct grpctts_job_conf *grpctts_job_conf_cpy(struct grpctts_job_conf *dest, const struct grpctts_job_conf *src)
{
	dest->speed = src->speed;
	dest->tone = src->tone;
	ast_free(dest->model);
	dest->model = ast_strdup(src->model);
	dest->initial_buffer_size = src->initial_buffer_size;
	return dest;
}


void grpctts_conf_init(struct grpctts_conf *conf)
{
    conf->endpoint = NULL;
    conf->token = NULL;
	grpctts_job_conf_init(&conf->job_conf);
}
void grpctts_conf_clear(struct grpctts_conf *conf)
{
    ast_free(conf->endpoint);
    ast_free(conf->token);
    conf->endpoint = NULL;
    conf->token = NULL;
	grpctts_job_conf_clear(&conf->job_conf);
}
int grpctts_conf_load(struct grpctts_conf *conf, ast_mutex_t *mutex, const char *fname, int reload)
{
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct ast_config *cfg = ast_config_load(fname, config_flags);
	if (!cfg) {
		if (mutex)
			ast_mutex_lock(mutex);
		grpctts_conf_clear(conf);
		if (mutex)
			ast_mutex_unlock(mutex);
		return 0;
	}
	if (cfg == CONFIG_STATUS_FILEUNCHANGED)
		return 0;
	if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_ERROR, "Config file vox_tts.conf is in an invalid format.  Aborting.\n");
		return -1;
	}

	if (mutex)
		ast_mutex_lock(mutex);

	grpctts_conf_clear(conf);

	char *cat = ast_category_browse(cfg, NULL);
	while (cat) {
		if (!strcasecmp(cat, "general") ) {
			struct ast_variable *var = ast_variable_browse(cfg, cat);
			while (var) {
                if (!strcasecmp(var->name, "endpoint")) {
                    ast_free(conf->endpoint);
                    conf->endpoint = ast_strdup(var->value);
                } else if (!strcasecmp(var->name, "token")) {
                        ast_free(conf->token);
                        conf->token = ast_strdup(var->value);
				} else if (!strcasecmp(var->name, "speed")) {
					char *eptr;
					double value = strtod(var->value, &eptr);
					if (*var->value && !*eptr)
						conf->job_conf.speed = value;
					else
						ast_log(AST_LOG_ERROR, "PlayBackground: parse error at '%s': invalid 'speed' value\n", fname);
				} else if (!strcasecmp(var->name, "tone")) {
					char *eptr;
					double value = strtod(var->value, &eptr);
					if (*var->value && !*eptr)
						conf->job_conf.tone = value;
					else
						ast_log(AST_LOG_ERROR, "PlayBackground: parse error at '%s': invalid 'tone' value\n", fname);
				} else if (!strcasecmp(var->name, "model")) {
					ast_free(conf->job_conf.model);
					conf->job_conf.model = ast_strdup(var->value);
				} else {
					ast_log(LOG_ERROR, "PlayBackground: parse error at '%s': category '%s': unknown keyword '%s' at line %d\n", fname, cat, var->name, var->lineno);
				}
				var = var->next;
			}
		} else if (!strcasecmp(cat, "buffering")) {
			struct ast_variable *var = ast_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "initial_buffer_size")) {
					struct grpctts_buffer_size initial_buffer_size;
					if (grpctts_parse_buffer_size(&initial_buffer_size, var->value)) {
						conf->job_conf.initial_buffer_size = initial_buffer_size;
					} else {
						ast_log(LOG_ERROR, "PlayBackground: parse error at '%s': category '%s': invalid buffer size specification '%s' at line %d\n",
							fname, cat, var->value, var->lineno);
					}
				} else {
					ast_log(LOG_ERROR, "PlayBackground: parse error at '%s': category '%s': unknown keyword '%s' at line %d\n", fname, cat, var->name, var->lineno);
				}
				var = var->next;
			}
		}
		cat = ast_category_browse(cfg, cat);
	}

	if (mutex)
		ast_mutex_unlock(mutex);
	ast_config_destroy(cfg);

	return 0;
}
struct grpctts_conf *grpctts_conf_cpy(struct grpctts_conf *dest, const struct grpctts_conf *src, ast_mutex_t *src_mutex)
{
    ast_free(dest->endpoint);
    ast_free(dest->token);
	ast_free(dest->job_conf.model);

	if (src_mutex)
		ast_mutex_lock(src_mutex);

	if (!grpctts_job_conf_cpy(&dest->job_conf, &src->job_conf)) {
		if (src_mutex)
			ast_mutex_unlock(src_mutex);
		grpctts_conf_init(dest);
		return NULL;
	}
    dest->endpoint = ast_strdup(src->endpoint);
    dest->token = ast_strdup(src->token);

	if (src_mutex)
		ast_mutex_unlock(src_mutex);

	return dest;
}
