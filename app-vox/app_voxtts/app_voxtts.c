extern struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#define _GNU_SOURCE 1
#include "stream_layers.h"
#include "grpctts.h"
#include "grpctts_conf.h"

#include <asterisk.h>

#include <asterisk/pbx.h>
#include <asterisk/app.h>
#include <asterisk/module.h>
#include <asterisk/manager.h>
#include <asterisk/utils.h>
#include <asterisk/dlinkedlists.h>
#include <asterisk/channel.h>
#include <asterisk/channel_internal.h>
#include <asterisk/mod_format.h>
#include <asterisk/format_cache.h>
#include <asterisk/paths.h>

#ifdef USE_EVENTFD
#include <sys/eventfd.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#include <sys/stat.h>
#include <math.h>

/*** DOCUMENTATION
	<application name="VoxPlayBackgroundInit" language="en_US">
		<synopsis>
			Initialize TTS channel with specified parameters in background.
		</synopsis>
		<syntax>
			<parameter name="conf_fname" required="false">
				<para>Specifies custom configuration filename for current TTS session (used by &quot;VoxPlayBackground(say,[OPTIONS],JSON_TASK)&quot;). By default&quot;voxtts.conf&quot; is used.</para>
			</parameter>
			<parameter name="endpoint" required="false">
				<para>Specifies endpointg for GRPC TTS service (must be specified here or at configuration file)</para>
			</parameter>
						<parameter name="token" required="false">
				<para>Specifies token for GRPC TTS service (must be specified here or at configuration file)</para>
			</parameter>
		</syntax>
		<description>
			<para>This application is necessary to allow &quot;VoxPlayBackgroundInit&quot; application call for speech synthesis using &quot;say,[OPTIONS],JSON_TASK&quot; command.</para>
		</description>
		<see-also>
			<ref type="application">VoxPlayBackground</ref>
		</see-also>
	</application>
	<application name="VoxPlayBackground" language="en_US">
		<synopsis>
			Execute playback commands in background.
		</synopsis>
		<syntax>
			<parameter name="[LAYER_N@][&amp;][COMMAND_NAME,OPTIONS,DATA]" required="true" />
		</syntax>
		<description>
			<para>This application will enqueue specified action sequence for playing in background.</para>
			<para>Each command is addressed to single audio layer which can be specified by 'LAYER_N@' prefix where LAYER_N is digit from 0 to 3 (default layer is 0).</para>
			<para>If command starts with '&amp;' character, following commands will be added to queue.</para>
			<para>Otherwise current playback will be stopped if present and following command will be added to queue and executed immediately.</para>
			<para>Available commands:</para>
			<para>- sleep,,TIMEOUT - pause for specified TIMEOUT seconds (specified as double precision floating point number)</para>
			<para>- say,[OPTION1=VALUE1[:OPTION2=VALUE2[...]]],INPUT - playback phrase specified by INPUT in JSON format and configured with OPTION=VALUE option set,
			for OPTIONS ',', ':', '(' and ')' characters must be backslash-escaped, INPUT must NOT be escaped</para>
			<para>- event,,EVENT - emit user event PlayBackgroundEvent with body EVENT</para>
			<para>When playback is finished at each layer empty frames are NOT being sent.</para>
			<para><emphasis>At each playback begin an quot;PlayBackgroundDuration(LAYER_N,DURATION_SECS)&quot; events are generated.</emphasis></para>
			<para><emphasis>At each playback end an &quot;PlayBackgroundFinished(LAYER_N)&quot; event is generated.</emphasis></para>
			<para><emphasis>At each event task reached an &quot;PlayBackgroundEvent(LAYER_N,EVENT)&quot; event is generated.</emphasis></para>
			<para><emphasis>At each playback error an &quot;PlayBackgroundError(LAYER_N)&quot; event is generated and remaining commands are dropped.</emphasis></para>
			<para><emphasis>Note that invocation with empty arguments will stop current playback.</emphasis></para>
			<example title="Say synthesized text using UTF-8">
			VoxPlayBackground(say,,{"text":"приветсвую вас на нашем канале"});
			</example>
		</description>
		<see-also>
			<ref type="application">VoxPlayBackgroundInit</ref>
		</see-also>
	</application>
 ***/

#define CONFIG_FILE "voxtts.conf"

static const char app_initgrpctts[] = "VoxPlayBackgroundInit";
static const char app[] = "VoxPlayBackground";

#define AUDIO_LAYER_COUNT 1




struct playback_control_message {
    char *command;
    AST_DLLIST_ENTRY(playback_control_message) list_meta;
};

struct ht_playback_layer_control {
    int override;
    AST_DLLIST_HEAD(entries, playback_control_message) entries;
};
struct ht_playback_control {
#ifdef USE_EVENTFD
    int eventfd;
#else
    int eventfd[2];
#endif

    ast_mutex_t mutex;
    struct grpctts_conf conf;
    struct ht_playback_layer_control layers[AUDIO_LAYER_COUNT];
    struct grpctts_channel *tts_channel;
};

static struct grpctts_conf dflt_grpctts_conf = GRPCTTS_CONF_INITIALIZER;
static ast_mutex_t dflt_grpctts_conf_mutex = AST_MUTEX_INIT_VALUE;


/* struct user_message methods */
static struct playback_control_message *make_playback_control_message(const char *command)
{
    size_t len = strlen(command);
    struct playback_control_message *s = ast_calloc(sizeof(struct playback_control_message) + len + 1, 1);
    if (!s)
        return NULL;
    s->command = memcpy((void*) (s + 1), command, len);
    s->command[len] = '\0';
    return s;
}

/* struct ht_user_message_queue methods */
static void clear_ht_playback_layer_control(struct ht_playback_layer_control *layer_control)
{
    struct playback_control_message *entry;
    while ((entry = AST_DLLIST_FIRST(&layer_control->entries))) {
        AST_DLLIST_REMOVE(&layer_control->entries, entry, list_meta);
        ast_free(entry);
    }
}
static void destroy_ht_playback_control(void *void_s)
{
    struct ht_playback_control *s = void_s;
    ast_mutex_destroy(&s->mutex);
    close(s->eventfd);
    {
        int i;
        for (i = 0; i < sizeof(s->layers)/sizeof(s->layers[0]); ++i)
            clear_ht_playback_layer_control(&s->layers[i]);
    }
    grpctts_channel_destroy(s->tts_channel);
    grpctts_conf_clear(&s->conf);
    ast_free(s);
}
static const struct ast_datastore_info playbackground_ds_info = {
        .type = "playback",
        .destroy = destroy_ht_playback_control,
};
static struct ht_playback_control *make_ht_playback_control(void)
{
    struct ht_playback_control *s = ast_calloc(sizeof(struct ht_playback_control), 1);
    if (!s)
        return NULL;
#ifdef USE_EVENTFD
    s->eventfd = eventfd(0, 0);
    fcntl(s->eventfd, F_SETFL, fcntl(s->eventfd, F_GETFL) | O_NONBLOCK);
#else
    pipe(s->eventfd);
    fcntl(s->eventfd[0], F_SETFL, fcntl(s->eventfd[0], F_GETFL) | O_NONBLOCK);
    fcntl(s->eventfd[1], F_SETFL, fcntl(s->eventfd[1], F_GETFL) | O_NONBLOCK);
    // ast_fd_set_flags(s->eventfd[0], O_NONBLOCK);
    //   ast_fd_set_flags(s->eventfd[1], O_NONBLOCK);
#endif

    ast_mutex_init(&s->mutex);
    grpctts_conf_init(&s->conf);
    grpctts_conf_cpy(&s->conf, &dflt_grpctts_conf, &dflt_grpctts_conf_mutex);
    return s;
}
static struct ht_playback_control *get_channel_control(struct ast_channel *chan)
{
    ast_channel_lock(chan);
    struct ast_datastore *datastore = ast_channel_datastore_find(chan, &playbackground_ds_info, NULL);
    if (!datastore) {
        ast_channel_unlock(chan);
        return NULL;
    }
    ast_channel_unlock(chan);
    return datastore->data;
}


static inline void eventfd_skip(int fd)
{
#ifdef USE_EVENTFD
    eventfd_t value;
#else
    char value;
#endif
    read(fd, &value, sizeof(value));
}

static void dispatch_jobs(struct stream_layer *layer, struct ht_playback_layer_control *layer_control, ast_mutex_t *mutex)
{
    ast_mutex_lock(mutex);
    if (layer_control->override) {
        stream_layer_override(layer);
        layer_control->override = 0;
    }
    struct playback_control_message *entry;
    while ((entry = AST_DLLIST_FIRST(&layer_control->entries))) {
        stream_layer_add_job(layer, entry->command);
        AST_DLLIST_REMOVE(&layer_control->entries, entry, list_meta);
        ast_free(entry);
    }
    ast_mutex_unlock(mutex);
}


static void *thread_routine(struct ast_channel *chan)
{
    struct ht_playback_control *control = get_channel_control(chan);
    if (!control)
        goto cleanup;

    ast_mutex_lock(&control->mutex);
#ifdef USE_EVENTFD
    int efd = control->eventfd;
#else
    int efd = control->eventfd[0];
#endif
    ast_mutex_unlock(&control->mutex);

    struct stream_layer layers[AUDIO_LAYER_COUNT];
    {
        int i;
        for (i = 0; i < sizeof(layers)/sizeof(layers[0]); ++i)
            stream_layer_init(&layers[i]);
    }

    struct stream_state state;
    stream_state_init(&state, chan, efd);

    while (!ast_check_hangup_locked(chan)) {
        /* Update local GRPC TTS value */
        ast_mutex_lock(&control->mutex);
        state.tts_channel = control->tts_channel;
        grpctts_job_conf_cpy(&state.job_conf, &control->conf.job_conf);
        ast_mutex_unlock(&control->mutex);

        int ret = stream_layers(&state, layers, sizeof(layers)/sizeof(layers[0]));
        if (ret == -1) {
            if (ast_channel_errno())
                ast_log(AST_LOG_ERROR, "Streaming error #%d\n", (int) ast_channel_errno());
            else
                ast_log(AST_LOG_DEBUG, "Channel closed\n");
            break;
        }
        if (ret == 1) {
            eventfd_skip(efd);
            if (ast_check_hangup_locked(chan))
                break;
            {
                int i;
                for (i = 0; i < sizeof(layers)/sizeof(layers[0]); ++i)
                    dispatch_jobs(&layers[i], &control->layers[i], &control->mutex);
            }
        }
    }

    cleanup:
    ast_channel_lock(chan);
    if (!ast_check_hangup(chan))
        ast_stopstream(chan);
    ast_channel_unlock(chan);
    {
        int i;
        for (i = 0; i < sizeof(layers)/sizeof(layers[0]); ++i)
            stream_layer_uninit(&layers[i]);
    }
    ast_channel_unref(chan);
    stream_state_uninit(&state);
    ast_log(AST_LOG_DEBUG, "PlayBackground application thread finished\n");
    return NULL;
}

static struct ht_playback_control *check_get_control(struct ast_channel *chan)
{
    struct ht_playback_control *control = get_channel_control(chan);
    if (control)
        return control;

    if (!(control = make_ht_playback_control()))
        return NULL;

    struct ast_datastore *datastore = ast_datastore_alloc(&playbackground_ds_info, NULL);
    if (!datastore) {
        destroy_ht_playback_control(control);
        return NULL;
    }

    datastore->data = control;

    ast_channel_lock(chan);
    ast_channel_datastore_add(chan, datastore);
    ast_channel_unlock(chan);

    ast_channel_ref(chan);

    pthread_t thread;
    ast_pthread_create_detached_background(&thread, NULL, (void*) thread_routine, chan);

    return control;
}
static int playbackgroundinitgrpctts_exec(struct ast_channel *chan, const char *data)
{
    struct ht_playback_control *control = check_get_control(chan);
    if (!control) {
        ast_log(LOG_ERROR, "Failed to initialize 'app_playbackground' control structure\n");
        return -1;
    }

    char *parse = ast_strdupa(data);
    AST_DECLARE_APP_ARGS(args,
                         AST_APP_ARG(conf_fname);
    AST_APP_ARG(endpoint);
    AST_APP_ARG(token);
    );

    AST_STANDARD_APP_ARGS(args, parse);

    if (args.conf_fname && *args.conf_fname) {
        grpctts_conf_clear(&control->conf);
        grpctts_conf_load(&control->conf, NULL, args.conf_fname, 0);
    }

    if (args.endpoint && *args.endpoint) {
        ast_free(control->conf.endpoint);
        control->conf.endpoint = ast_strdup(args.endpoint);
    }
    if (args.token && *args.token) {
        ast_free(control->conf.token);
        control->conf.token = ast_strdup(args.token);
    }

    if (!control->conf.endpoint) {
        ast_log(LOG_ERROR, "PlayBackgroundInitGRPCTTS: Failed to execute application: no endpoint specified\n");
        return -1;
    }

    ast_mutex_lock(&control->mutex);
    if (!control->tts_channel)
        control->tts_channel = grpctts_channel_create(control->conf.endpoint, control->conf.token);
    ast_mutex_unlock(&control->mutex);

    return 0;
}
static int playbackground_exec(struct ast_channel *chan, const char *data)
{
    struct ht_playback_control *control = check_get_control(chan);
    if (!control) {
        ast_log(LOG_ERROR, "Failed to initialize 'app_playbackground' control structure\n");
        return -1;
    }

    ast_mutex_lock(&control->mutex);
    struct ht_playback_layer_control *layer_control = &control->layers[0];
    if (data[0] == '0' && data[1] == '@') {
        data += 2;
    } else if (data[0] == '1' && data[1] == '@') {
        layer_control = &control->layers[1];
        data += 2;
    } else if (data[0] == '2' && data[1] == '@') {
        layer_control = &control->layers[2];
        data += 2;
    } else if (data[0] == '3' && data[1] == '@') {
        layer_control = &control->layers[3];
        data += 2;
    }
    int empty = 0;
    if (!*data) {
        /* Empty string */
        empty = 1;
        layer_control->override = 1;
    } else if (*data == '&') {
        /* Append commands */
        ++data;
    } else {
        /* Replace commands */
        struct playback_control_message *entry;
        while ((entry = AST_DLLIST_FIRST(&layer_control->entries))) {
            AST_DLLIST_REMOVE(&layer_control->entries, entry, list_meta);
            ast_free(entry);
        }
        layer_control->override = 1;
    }
    if (!empty) {
        struct playback_control_message *entry = make_playback_control_message(data);
        if (entry) {
            AST_DLLIST_INSERT_TAIL(&layer_control->entries, entry, list_meta);
        } else {
            ast_log(LOG_ERROR, "Failed to create 'app_playbackground' control message\n");
        }
    }
#ifdef USE_EVENTFD
    eventfd_write(control->eventfd, 1);
#else
    write(control->eventfd[1], "1", 1);
#endif
    ast_mutex_unlock(&control->mutex);

    return 0;
}
static void stream_error_callback(const char *message)
{
    ast_log(LOG_ERROR, "%s\n", message);
}

static int unload_module(void)
{
    stream_layers_global_uninit();
    grpctts_shutdown();
    grpctts_conf_global_uninit();
    return ast_unregister_application(app);
}

static int load_module(void)
{
    grpctts_conf_global_init();
    grpctts_set_stream_error_callback(stream_error_callback);
    grpctts_init();
    stream_layers_global_init();
    if (grpctts_conf_load(&dflt_grpctts_conf, &dflt_grpctts_conf_mutex, CONFIG_FILE, 0))
        return AST_MODULE_LOAD_DECLINE;
    return
            ast_register_application_xml(app_initgrpctts, playbackgroundinitgrpctts_exec) |
            ast_register_application_xml(app, playbackground_exec);
}

static int reload(void)
{
    if (grpctts_conf_load(&dflt_grpctts_conf, &dflt_grpctts_conf_mutex, CONFIG_FILE, 1))
        return AST_MODULE_LOAD_DECLINE;
    return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "[" ASTERISK_MODULE_VERSION_STRING "] Background Playback Application",
.support_level = AST_MODULE_SUPPORT_EXTENDED,
.load = load_module,
.unload = unload_module,
.reload = reload,
);
