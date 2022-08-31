extern struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#include "grpc_stt.h"

#include <grpc/grpc.h>

#include <asterisk.h>
#include <asterisk/pbx.h>
#include <asterisk/app.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>
#include <asterisk/utils.h>
#include <asterisk/astobj2.h>
#include <asterisk/dlinkedlists.h>
#include <asterisk/format_cache.h>
#include <asterisk/paths.h>
#include <asterisk/alaw.h>

#ifdef USE_EVENTFD
#include <sys/eventfd.h>
#else
#include <unistd.h>
#endif

#include <sys/select.h>
#include <sys/stat.h>
#include <math.h>

/*** DOCUMENTATION
	<application name="VoxASRBackground" language="en_US">
		<synopsis>
			Recognize incoming channel speech into text.
		</synopsis>
		<syntax>
			<parameter name="endpoint" required="true">
				<para>Specifies service endpoint with HOST:PORT format</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="A">
						<para>Encode UTF-8 characters as ASCII escape sequence at generated JSON events</para>
					</option>
				</optionlist>
			</parameter>
			<parameter name="model">
				<para>Specifies model for ASR session</para>
			</parameter>
		</syntax>
		<description>
			<para>This application connects to ASR service at specified endpoint.</para>
			<para>It then sends incoming channel audio frames and receives recognized text.</para>
			<para>Upon each received recognized phrase a channel user event is generated wich may be catched with WaitEvent() application or AMI subsystem.</para>
			<para>Following events are generated (event body is specified inside braces):</para>
			<para><emphasis>At receiving ASR recognition hypothesis &quot;VOX_ASR_JSON_ASCII(JSON)&quot;, &quot;VOX_ASR_JSON_UTF8(JSON)&quot; and &quot;VOX_ASR_TEXT(TEXT)&quot; events are generated.</emphasis></para>
			<para><emphasis>At session close an &quot;VOX_ASR_SESSION_FINISHED(STATUS,ERROR_CODE,ERROR_MESSAGE)&quot; event is generated.</emphasis></para>
			<example title="Start streaming to ASR at domain.org:300">
			 VoxASRBackground(domain.org:300);
			</example>
			<example title="Get next event and print details if event is VOX_ASR_SESSION_FINISHED">
			 WaitEvent(${SLEEP_TIME});
			 if (${WAITEVENTNAME} == VOX_ASR_SESSION_FINISHED) {
			         Set(ARRAY(STATUS,ERROR_CODE,ERROR_MESSAGE)=${WAITEVENTBODY});
			         if (${STATUS} == SUCCESS) {
			                 Log(NOTICE,Session finished successfully);
			         } else {
			                 Log(NOTICE,Session finished with error ${ERROR_CODE}: ${ERROR_MESSAGE});
			         }
			 }
			</example>
		</description>
		<see-also>
			<ref type="application">WaitEvent</ref>
			<ref type="application">WaitEventInit</ref>
			<ref type="application">VoxASRBackgroundFinish</ref>
		</see-also>
	</application>
	<application name="VoxASRBackgroundFinish" language="en_US">
		<synopsis>
			Finish speech recognition session.
		</synopsis>
		<description>
			<para>This application terminates speech recognition session previously runned by VoxASRBackground().</para>
			<para>It is safe to call VoxASRBackgroundFinish() even if no VoxASRBackground() was previously called.</para>
		</description>
		<see-also>
			<ref type="application">VoxASRBackground</ref>
		</see-also>
	</application>
 ***/


static const char app[] = "VoxASRBackground";
static const char app_finish[] = "VoxASRBackgroundFinish";

static ast_mutex_t dflt_thread_conf_mutex;



struct thread_conf {
    int terminate_event_fd;
#ifndef USE_EVENTFD
    int terminate_event_fd_out;
#endif

    struct ast_channel *chan;
    char *endpoint;
    char *model;
};

static struct thread_conf dflt_thread_conf = {
        .terminate_event_fd = -1,
#ifndef USE_EVENTFD
        .terminate_event_fd_out = -1,
#endif
        .chan = NULL,
        .endpoint = NULL,
        .model = NULL,
};



static void *thread_start(struct thread_conf *conf)
{
    struct ast_channel *chan = conf->chan;
    grpc_stt_run(conf->terminate_event_fd
#ifndef USE_EVENTFD
    , conf->terminate_event_fd_out
#endif
    , conf->endpoint, chan,conf->model);

    close(conf->terminate_event_fd);
#ifndef USE_EVENTFD
    close(conf->terminate_event_fd_out);
#endif
    ast_channel_unref(chan);
    ast_free(conf);
    return NULL;
}


static void clear_config(void)
{
    ast_free(dflt_thread_conf.endpoint);
    ast_free(dflt_thread_conf.model);
    dflt_thread_conf.chan = NULL;
    dflt_thread_conf.endpoint = NULL;
    dflt_thread_conf.model = NULL;
}


static int load_config(int reload)
{
    struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
    struct ast_config *cfg = ast_config_load("vox.conf", config_flags);
    if (!cfg) {
        ast_mutex_lock(&dflt_thread_conf_mutex);
        clear_config();
        ast_mutex_unlock(&dflt_thread_conf_mutex);
        return 0;
    }
    if (cfg == CONFIG_STATUS_FILEUNCHANGED)
        return 0;
    if (cfg == CONFIG_STATUS_FILEINVALID) {
        ast_log(LOG_ERROR, "Config file vox.conf is in an invalid format.  Aborting.\n");
        return -1;
    }

    ast_mutex_lock(&dflt_thread_conf_mutex);

    clear_config();

    char *cat = ast_category_browse(cfg, NULL);
    while (cat) {
        if (!strcasecmp(cat, "general") ) {
            struct ast_variable *var = ast_variable_browse(cfg, cat);
            while (var) {
                if (!strcasecmp(var->name, "endpoint")) {
                    dflt_thread_conf.endpoint = ast_strdup(var->value);
                } else if (!strcasecmp(var->name, "model")) {
                    dflt_thread_conf.model = ast_strdup(var->value);
                } else {
                    ast_log(LOG_WARNING, "%s: Cat:%s. Unknown keyword %s at line %d of vox.conf\n", app, cat, var->name, var->lineno);
                }
                var = var->next;
            }

        }
        cat = ast_category_browse(cfg, cat);
    }

    ast_mutex_unlock(&dflt_thread_conf_mutex);
    ast_config_destroy(cfg);

    return 0;
}




static struct thread_conf *make_thread_conf(const struct thread_conf *source)
{
    size_t endpoint_len = strlen(source->endpoint) + 1;
    size_t model_len = source->model ? (strlen(source->model) + 1) : 0;
    struct thread_conf *conf = ast_malloc(sizeof(struct thread_conf) + endpoint_len + model_len);
    if (!conf)
        return NULL;
    void *p = conf + 1;
    conf->terminate_event_fd = -1;
#ifndef USE_EVENTFD
    conf->terminate_event_fd_out = -1;
#endif

    conf->chan = source->chan;

    conf->endpoint = strcpy(p, source->endpoint);
    p += endpoint_len;
    conf->model = source->model ? strcpy(p, source->model) : NULL;
    return conf;
}


struct grpcsttbackground_control {
    int terminate_event_fd;
#ifndef USE_EVENTFD
    int terminate_event_fd_out;
#endif
};
static struct grpcsttbackground_control *make_grpcsttbackground_control(int terminate_event_fd
#ifndef USE_EVENTFD
        , int terminate_event_fd_out
#endif
        )
{
    struct grpcsttbackground_control *s = ast_calloc(sizeof(struct grpcsttbackground_control), 1);
    if (!s)
        return NULL;
    s->terminate_event_fd = terminate_event_fd;
#ifndef USE_EVENTFD
    s->terminate_event_fd_out= terminate_event_fd_out;
#endif

    return s;
}
static void destroy_grpcsttbackground_control(void *void_s)
{
    struct grpcsttbackground_control *s = void_s;

#ifdef USE_EVENTFD
    eventfd_write(s->terminate_event_fd, 1);
#else
    write(s->terminate_event_fd_out, "1", 1);
#endif
    close(s->terminate_event_fd);

    ast_free(s);
}
static const struct ast_datastore_info grpcsttbackground_ds_info = {
        .type = "grpcsttbackground",
        .destroy = destroy_grpcsttbackground_control,
};

static void clear_channel_control_state_unlocked(struct ast_channel *chan)
{
    struct ast_datastore *datastore = ast_channel_datastore_find(chan, &grpcsttbackground_ds_info, NULL);
    if (datastore) {
        ast_channel_datastore_remove(chan, datastore);
        ast_datastore_free(datastore);
    }
}
static void clear_channel_control_state(struct ast_channel *chan)
{
    ast_channel_lock(chan);
    clear_channel_control_state_unlocked(chan);
    ast_channel_unlock(chan);
}

static void replace_channel_control_state_unlocked(struct ast_channel *chan, int terminate_event_fd
#ifndef USE_EVENTFD
        , int terminate_event_fd_out
#endif
        )
{
    clear_channel_control_state_unlocked(chan);

    struct grpcsttbackground_control *control = make_grpcsttbackground_control(terminate_event_fd
#ifndef USE_EVENTFD
            , terminate_event_fd_out
#endif
        );
    if (!control)
        return;
    struct ast_datastore *datastore = ast_datastore_alloc(&grpcsttbackground_ds_info, NULL);
    if (!datastore) {
        destroy_grpcsttbackground_control(control);
        return;
    }
    datastore->data = control;
    ast_channel_datastore_add(chan, datastore);
}
static void replace_channel_control_state(struct ast_channel *chan, int terminate_event_fd
#ifndef USE_EVENTFD
        , int terminate_event_fd_out
#endif
        )
{
    ast_channel_lock(chan);
    replace_channel_control_state_unlocked(chan, terminate_event_fd
#ifndef USE_EVENTFD
            , terminate_event_fd_out
#endif
);
    ast_channel_unlock(chan);
}

static int make_event_fd_pair(int *parent_fd_p, int *child_fd_p
#ifndef USE_EVENTFD
                              , int *fd_out
#endif
)
{
#ifdef USE_EVENTFD
    int parent_fd = eventfd(0, 0);
    if (parent_fd < 0) {
        *parent_fd_p = -1;
        *child_fd_p = -1;
        return -1;
    }
    int child_fd = dup(parent_fd);
    if (child_fd < 0) {
        int saved_errno = errno;
        close(parent_fd);
        *parent_fd_p = -1;
        *child_fd_p = -1;
        errno = saved_errno;
        return -1;
    }
    *parent_fd_p = parent_fd;
    *child_fd_p = child_fd;
    return 0;
#else
    int parent[2];
    int res = pipe(parent);
    if (res < 0 ) {
        *parent_fd_p = -1;
        *child_fd_p = -1;
        *fd_out = -1;
        return -1;
    }
    int child_fd = dup(parent[0]);
    if (child_fd < 0) {
        int saved_errno = errno;
        close(parent[0]);
        close(parent[1]);
        *parent_fd_p = -1;
        *child_fd_p = -1;
        *fd_out = -1;
        errno = saved_errno;
        return -1;
    }
    *parent_fd_p = parent[0];
    *child_fd_p = child_fd;
    *fd_out = parent[1];
    return 0;
#endif
}

static int grpcsttbackground_exec(struct ast_channel *chan, const char *data)
{
    ast_mutex_lock(&dflt_thread_conf_mutex);
    struct thread_conf thread_conf = dflt_thread_conf;
    thread_conf.chan = chan;

    char *parse = ast_strdupa(data);
    AST_DECLARE_APP_ARGS(args,
                         AST_APP_ARG(endpoint);
    AST_APP_ARG(model);
    );

    AST_STANDARD_APP_ARGS(args, parse);

    if (args.endpoint && *args.endpoint)
        thread_conf.endpoint = args.endpoint;

    if (!thread_conf.endpoint) {
        ast_log(LOG_ERROR, "%s: Failed to execute application: no endpoint specified\n", app);
        ast_mutex_unlock(&dflt_thread_conf_mutex);
        return -1;
    }
    if (args.model && *args.model) {
        thread_conf.model = args.model;
    }

    ast_channel_ref(chan);
    struct thread_conf *conf = make_thread_conf(&thread_conf);
    ast_mutex_unlock(&dflt_thread_conf_mutex);
    if (!conf) {
        ast_channel_unref(chan);
        return -1;
    }
    int terminate_event_fd, child_terminate_event_fd;
#ifndef USE_EVENTFD
    int terminate_event_fd_out;
#endif
    if (make_event_fd_pair(&terminate_event_fd, &child_terminate_event_fd
#ifndef USE_EVENTFD
            , &terminate_event_fd_out
#endif
)) {
        ast_channel_unref(chan);
        return -1;
    }
    conf->terminate_event_fd = child_terminate_event_fd;
#ifndef USE_EVENTFD
    conf->terminate_event_fd = terminate_event_fd_out;
#endif
    pthread_t thread;
    if (ast_pthread_create_detached_background(&thread, NULL, (void *) thread_start, conf)) {
        ast_log(AST_LOG_ERROR, "Failed to start thread\n");
        ast_channel_unref(chan);
        close(terminate_event_fd);
        close(child_terminate_event_fd);
        return -1;
    }
    replace_channel_control_state(chan, terminate_event_fd
#ifndef USE_EVENTFD
            , terminate_event_fd_out
#endif
    );
    return 0;
}

static int grpcsttbackgroundfinish_exec(struct ast_channel *chan, const char *data)
{
    clear_channel_control_state(chan);
    return 0;
}


static int load_module(void)
{
    grpc_init();
    if (load_config(0) || (
            ast_register_application_xml(app, grpcsttbackground_exec) |
            ast_register_application_xml(app_finish, grpcsttbackgroundfinish_exec) ))
        return AST_MODULE_LOAD_DECLINE;
    return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
    grpc_shutdown();
    ast_mutex_lock(&dflt_thread_conf_mutex);
    clear_config();
    ast_mutex_unlock(&dflt_thread_conf_mutex);
    return ast_unregister_application(app) | ast_unregister_application(app_finish);

}


static int reload(void)
{
    if (load_config(1))
        return AST_MODULE_LOAD_DECLINE;
    return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "[" ASTERISK_MODULE_VERSION_STRING "] VoxASRBackground Application",
.support_level = AST_MODULE_SUPPORT_EXTENDED,
.load = load_module,
.unload = unload_module,
.reload = reload,
);
