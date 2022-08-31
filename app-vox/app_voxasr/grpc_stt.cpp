extern "C" struct ast_module *AST_MODULE_SELF_SYM(void);
#define AST_MODULE_SELF_SYM AST_MODULE_SELF_SYM

#define typeof __typeof__
#include "asr_api.grpc.pb.h"
#include "grpc_stt.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
extern "C" {
#include <asterisk.h>
#include <asterisk/autoconfig.h>
#include <asterisk/compiler.h>
#include <asterisk/time.h>
#include <asterisk/channel.h>
#include <asterisk/format_cache.h>
#include <asterisk/alaw.h>
#include <asterisk/ulaw.h>
#include <fcntl.h>
#include <jansson.h>

#ifdef USE_EVENTFD
#include <sys/eventfd.h>
#else
#include <unistd.h>
#include <asterisk/utils.h>
#endif

}

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC_PRECISE
#endif
// 7 days
#define EXPIRATION_PERIOD (7*86400)

#define INTERNAL_SAMPLE_RATE 8000
#define MAX_FRAME_DURATION_MSEC 100
#define MAX_FRAME_SAMPLES 800
#define ALIGNMENT_SAMPLES 80



static inline int delta_samples(const struct timespec *a, const struct timespec *b)
{
	struct timespec delta;
	delta.tv_sec = a->tv_sec - b->tv_sec;
	delta.tv_nsec = a->tv_nsec - b->tv_nsec;
	if (delta.tv_nsec < 0) {
		delta.tv_sec--;
		delta.tv_nsec += 1000000000;
	}
	
	return delta.tv_sec*INTERNAL_SAMPLE_RATE + ((int64_t) delta.tv_nsec)*INTERNAL_SAMPLE_RATE/1000000000;
}
static inline void time_add_samples(struct timespec *t, int samples)
{
	t->tv_sec += samples/INTERNAL_SAMPLE_RATE;
	t->tv_nsec += ((int64_t) (samples%INTERNAL_SAMPLE_RATE))*1000000000/INTERNAL_SAMPLE_RATE;
	if (t->tv_nsec >= 1000000000) {
		t->tv_sec++;
		t->tv_nsec -= 1000000000;
	}
}
static inline int aligned_samples(int samples)
{
	return (samples + ALIGNMENT_SAMPLES/2)/ALIGNMENT_SAMPLES*ALIGNMENT_SAMPLES;
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

static json_t *build_json_duration(const google::protobuf::Duration &duration)
{
	json_t *json_duration = json_object();
	json_object_set_new_nocheck(json_duration, "seconds", json_real(duration.seconds()));
	json_object_set_new_nocheck(json_duration, "nanos", json_real(duration.nanos()));
	return json_duration;
}

static void push_grpcstt_event(struct ast_channel *chan, const std::string &data, bool ensure_ascii)
{
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", (ensure_ascii ? "VOX_ASR_JSON_ASCII" : "VOX_ASR_JSON_UTF8"), "eventbody", data.c_str());
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);
	ast_json_unref(blob);
}

static void push_grpcstt_event2(struct ast_channel *chan, const std::string &data)
{
    struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "VOX_ASR_TEXT", "eventbody", data.c_str());
    if (!blob)
        return;

    ast_channel_lock(chan);
    ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
    ast_channel_unlock(chan);
    ast_json_unref(blob);
}

static std::string build_grpcstt_event(const vox::asr::StreamingRecognitionResponse& response, bool json_ensure_ascii)
{

    json_t *json_root = json_object();
    json_object_set_new_nocheck(json_root, "is_final", json_boolean(response.final()));
    json_object_set_new_nocheck(json_root, "transcript", json_string(response.text().c_str()));
    json_t *json_chunks = json_array();
    for (const vox::asr::SpeechRecognitionChunk &chunk: response.chunks()) {
        json_t *json_chunk = json_object();
        json_t *json_words = json_array();

        for (const std::string &word: chunk.words()) {
            json_array_append_new(json_words, json_string(word.c_str()));
        }
        json_object_set_new_nocheck(json_chunk, "words", json_words);
        json_object_set_new_nocheck(json_chunk, "confidence", json_real(chunk.confidence()));
        json_object_set_new_nocheck(json_chunk, "loudness", json_real(chunk.loudness()));
        json_object_set_new_nocheck(json_chunk, "start_time", build_json_duration(chunk.start_time()));
        json_object_set_new_nocheck(json_chunk, "end_time", build_json_duration(chunk.end_time()));

        json_array_append_new(json_chunks, json_chunk);
    }
    json_object_set_new_nocheck(json_root, "chunks", json_chunks);
    char *dump = json_dumps(json_root, (json_ensure_ascii ? (JSON_COMPACT | JSON_ENSURE_ASCII) : (JSON_COMPACT)));
    std::string result(dump);
    ast_json_free(dump);
    json_decref(json_root);
    return result;
}

static void push_grpcstt_session_finished_event(struct ast_channel *chan, bool success, int error_code, const std::string &error_message)
{
	std::string data = success ? "SUCCESS,," : ("FAILURE," + std::to_string(error_code) + "," + error_message);
	struct ast_json *blob = ast_json_pack("{s: s, s: s}", "eventname", "VOX_ASR_SESSION_FINISHED", "eventbody", data.c_str());
	if (!blob)
		return;

	ast_channel_lock(chan);
	ast_multi_object_blob_single_channel_publish(chan, ast_multi_user_event_type(), blob);
	ast_channel_unlock(chan);

	ast_json_unref(blob);
}
static const char *get_frame_samples(struct ast_frame *f, std::vector<uint8_t> &buffer, size_t *len, bool *warned)
{
	size_t sample_count = f->samples;
	const char *data = NULL;
    *len = sample_count*sizeof(int16_t);
    if (f->subclass.format == ast_format_alaw) {
        buffer.resize(sample_count*sizeof(int16_t));
        int16_t *dptr = (int16_t *) buffer.data();
        uint8_t *sptr = (uint8_t *) f->data.ptr;
        for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
            int16_t slin_sample = AST_ALAW(*sptr);
            *dptr = htole16(slin_sample);
        }
        data = (const char *) buffer.data();
    } else if (f->subclass.format == ast_format_ulaw) {
        buffer.resize(sample_count*sizeof(int16_t));
        int16_t *dptr = (int16_t *) buffer.data();
        uint8_t *sptr = (uint8_t *) f->data.ptr;
        for (size_t i = 0; i < sample_count; ++i, ++dptr, ++sptr) {
            int16_t slin_sample = AST_MULAW(*sptr);
            *dptr = htole16(slin_sample);
        }
        data = (const char *) buffer.data();
    } else if (f->subclass.format == ast_format_slin) {
        data = (const char *) f->data.ptr;
    } else {
        if (!warned) {
            ast_log(AST_LOG_WARNING, "Unhandled frame format, ignoring!\n");
            *warned = true;
        }
    }

	return data;
}
static std::vector<uint8_t> make_silence_samples(size_t samples)
{
    return std::vector<uint8_t>(samples*sizeof(int16_t), 0);
}


AST_LIST_HEAD(grpcstt_frame_list, ast_frame);

class GRPCSTT
{
public:
	static void AttachToChannel(std::shared_ptr<GRPCSTT> &grpc_stt);
	static void DetachFromChannel(std::shared_ptr<GRPCSTT> &grpc_stt) noexcept;

public:
	GRPCSTT(int terminate_event_fd,
#ifndef USE_EVENTFD
            int terminate_event_fd_out,
#endif
            std::shared_ptr<grpc::Channel> grpc_channel, struct ast_channel *chan, const char *model);
	~GRPCSTT();
	void ReapAudioFrame(struct ast_frame *frame);
	void Terminate() noexcept;
	bool Run(int &error_status, std::string &error_message);

private:
	std::unique_ptr<vox::asr::SttService::Stub> stt_stub;
	struct ast_channel *chan;
	std::string model;

    int terminate_event_fd;
#ifdef USE_EVENTFD
    int frame_event_fd;
#else
    int terminate_event_fd_out;
    int frame_event_fd[2];
#endif

	struct grpcstt_frame_list audio_frames;
	int framehook_id;
};


static struct ast_frame *framehook_event_callback (struct ast_channel *chan, struct ast_frame *frame, enum ast_framehook_event event, void *data)
{
	if (frame) {
		if (event == AST_FRAMEHOOK_EVENT_READ)
			(*(std::shared_ptr<GRPCSTT>*) data)->ReapAudioFrame(frame);
	}

	return frame;
}
static int framehook_consume_callback (void *data, enum ast_frame_type type)
{
	return 0;
}
static void framehook_destroy_callback (void *data)
{
	(*(std::shared_ptr<GRPCSTT>*) data)->Terminate();
	delete (std::shared_ptr<GRPCSTT>*) data;
}


void GRPCSTT::AttachToChannel(std::shared_ptr<GRPCSTT> &grpc_stt)
{
	struct ast_framehook_interface interface = {.version = AST_FRAMEHOOK_INTERFACE_VERSION};
	interface.event_cb = framehook_event_callback;
	interface.consume_cb = framehook_consume_callback;
	interface.destroy_cb = framehook_destroy_callback;
	interface.data = (void*) new std::shared_ptr<GRPCSTT>(grpc_stt);
	ast_channel_lock(grpc_stt->chan);
	int id = ast_framehook_attach(grpc_stt->chan, &interface);
	ast_channel_unlock(grpc_stt->chan);
	grpc_stt->framehook_id = id;
}
void GRPCSTT::DetachFromChannel(std::shared_ptr<GRPCSTT> &grpc_stt) noexcept
{
	int id = grpc_stt->framehook_id;
	if (id == -1)
		return;
	ast_channel_lock(grpc_stt->chan);
	ast_framehook_detach(grpc_stt->chan, id);
	ast_channel_unlock(grpc_stt->chan);
	grpc_stt->framehook_id = -1;
}
GRPCSTT::GRPCSTT(int terminate_event_fd,
#ifndef USE_EVENTFD
        int terminate_event_fd_out,
#endif
                 std::shared_ptr<grpc::Channel> grpc_channel,
		 struct ast_channel *chan, const char *model)
	:     stt_stub(vox::asr::SttService::NewStub(grpc_channel)),
          chan(chan), model(model),
    terminate_event_fd(terminate_event_fd),
#ifndef USE_EVENTFD
        terminate_event_fd_out(terminate_event_fd_out),
#endif
 framehook_id(-1)
{
#ifdef USE_EVENTFD
    frame_event_fd = eventfd(0, 0);
    fcntl(frame_event_fd, F_SETFL, fcntl(frame_event_fd, F_GETFL) | O_NONBLOCK);
#else
    pipe(frame_event_fd);
    fcntl(frame_event_fd[0], F_SETFL, fcntl(frame_event_fd[0], F_GETFL) | O_NONBLOCK);
    fcntl(frame_event_fd[1], F_SETFL, fcntl(frame_event_fd[1], F_GETFL) | O_NONBLOCK);

// ast_fd_set_flags(frame_event_fd[0], O_NONBLOCK);
 //   ast_fd_set_flags(frame_event_fd[1], O_NONBLOCK);
#endif

	AST_LIST_HEAD_INIT(&audio_frames);
}
GRPCSTT::~GRPCSTT()
{
#ifdef USE_EVENTFD
    close(frame_event_fd);
#else
    close(frame_event_fd[0]);
    close(frame_event_fd[1]);
#endif

	AST_LIST_LOCK(&audio_frames);
	struct ast_frame *f;
	while ((f = AST_LIST_REMOVE_HEAD(&audio_frames, frame_list)))
		ast_frame_dtor(f);
	AST_LIST_UNLOCK(&audio_frames);
}

void GRPCSTT::ReapAudioFrame(struct ast_frame *frame)
{
	struct ast_frame *f = ast_frdup(frame);
	if (!f)
		return;

	AST_LIST_LOCK(&audio_frames);
	AST_LIST_INSERT_TAIL(&audio_frames, f, frame_list);
	AST_LIST_UNLOCK(&audio_frames);

#ifdef USE_EVENTFD
    eventfd_write(frame_event_fd, 1);
#else
    write(frame_event_fd[1], "1", 1);
#endif

}
void GRPCSTT::Terminate() noexcept
{
#ifdef USE_EVENTFD
    eventfd_write(terminate_event_fd, 1);
#else
    write(terminate_event_fd_out, "1", 1);
#endif
}
bool GRPCSTT::Run(int &error_status, std::string &error_message)
{
	error_status = 0;
	error_message = "";

	grpc::ClientContext context;
	std::shared_ptr<grpc::ClientReaderWriter<vox::asr::StreamingRecognitionRequest,
            vox::asr::StreamingRecognitionResponse >> stream(stt_stub->StreamingRecognize(&context));


	try {
		std::thread writer(
			[stream, this]()
			{

                {
                    vox::asr::StreamingRecognitionRequest initial_request;
                    vox::asr::RecognitionConfig *recognition_config = initial_request.mutable_config();
                    recognition_config->set_model(model);
					stream->Write(initial_request);
				}
			}
		);
		writer.join();
	} catch (const std::exception &ex) {
		error_status = -1;
		error_message = std::string("VOX ASR finished with error: ") + ex.what();
		return false;
	} catch (...) {
		error_status = -1;
		error_message = "VOX ASR finished with unknown error";
		return false;
	}

	std::thread writer(
		[stream, this]()
		{
			bool stream_valid = true;
			bool warned = false;
			struct timespec last_frame_moment;
			clock_gettime(CLOCK_MONOTONIC_RAW, &last_frame_moment);
			while (stream_valid && !ast_check_hangup_locked(chan)) {
				struct pollfd pfds[2] = {
					{
                        .fd = terminate_event_fd,
						.events = POLLIN,
						.revents = 0,
					},
					{
#ifdef USE_EVENTFD
                                .fd = frame_event_fd,
#else
                                .fd = frame_event_fd[0],
#endif

						.events = POLLIN,
						.revents = 0,
					},
				};
				poll(pfds, 2, MAX_FRAME_DURATION_MSEC*2);
				if (pfds[0].revents & POLLIN)
					break;
				if (!(pfds[1].revents & POLLIN)) {
					struct timespec current_moment;
					clock_gettime(CLOCK_MONOTONIC_RAW, &current_moment);
					int gap_samples = aligned_samples(delta_samples(&current_moment, &last_frame_moment) - MAX_FRAME_SAMPLES);
					if (gap_samples > 0) {
						vox::asr::StreamingRecognitionRequest request;
						std::vector<uint8_t> buffer = make_silence_samples(gap_samples);
						request.set_audio_content(buffer.data(), buffer.size());
                        request.set_only_new(true);
						if (!stream->Write(request))
							stream_valid = false;
						time_add_samples(&last_frame_moment, gap_samples);
					}
					continue;
				}
#ifdef USE_EVENTFD
                eventfd_skip(frame_event_fd);
#else
                eventfd_skip(frame_event_fd[0]);
#endif



				bool gap_handled = false;
				while (stream_valid) {
					AST_LIST_LOCK(&audio_frames);
					struct ast_frame *f = AST_LIST_REMOVE_HEAD(&audio_frames, frame_list);
					AST_LIST_UNLOCK(&audio_frames);
					if (!f)
						break;

					if (f->frametype == AST_FRAME_VOICE) {
						struct timespec current_moment;
						clock_gettime(CLOCK_MONOTONIC_RAW, &current_moment);
						if (!gap_handled) {
							int gap_samples = aligned_samples(delta_samples(&current_moment, &last_frame_moment) - f->samples);
							if (gap_samples > 0) {
                                vox::asr::StreamingRecognitionRequest request;
								std::vector<uint8_t> buffer = make_silence_samples(gap_samples);
								request.set_audio_content(buffer.data(), buffer.size());
                                request.set_only_new(true);

                                if (!stream->Write(request))
									stream_valid = false;
								time_add_samples(&last_frame_moment, gap_samples);
							}
							gap_handled = true;
						}

                        vox::asr::StreamingRecognitionRequest request;
						std::vector<uint8_t> buffer;
						size_t len = 0;
						const char *data = get_frame_samples(f, buffer, &len, &warned);
						if (data) {
							time_add_samples(&last_frame_moment, f->samples);
							request.set_audio_content(data, len);
                            request.set_only_new(true);
                            if (!stream->Write(request))
								stream_valid = false;
						}
					}
					ast_frame_dtor(f);
				}
			}
			stream->WritesDone();
		}
	);

	try {
        vox::asr::StreamingRecognitionResponse response;
		while (stream->Read(&response)) {
            push_grpcstt_event(chan, build_grpcstt_event(response, false), false);
            push_grpcstt_event(chan, build_grpcstt_event(response, true), true);
            push_grpcstt_event2(chan, response.text());
		}
	} catch (const std::exception &ex) {
		Terminate();
		writer.join();
		error_status = -1;
		error_message = std::string("VOX ASR finished with error: ") + ex.what();
		return false;
	} catch (...) {
        Terminate();
		writer.join();
		error_status = -1;
		error_message = "VOX ASR finished with unknown error";
		return false;
	}
    Terminate();
	writer.join();
	grpc::Status status = stream->Finish();
	if (!status.ok()) {
        error_status = status.error_code();
		error_message = "VOX ASR finished with error (code = " + std::to_string(status.error_code()) + "): " + std::string(status.error_message());
		return false;
	}
    return true;
}


extern "C" void grpc_stt_run(
        int terminate_event_fd,
#ifndef USE_EVENTFD
       int terminate_event_fd_out,
#endif
        const char *endpoint,
			     struct ast_channel *chan, const char *model)
{
	bool success = false;
	int error_status;
	std::string error_message;
	try {
#define NON_NULL_STRING(str) ((str) ? (str) : "")
		std::shared_ptr<GRPCSTT> grpc_stt = std::make_shared<GRPCSTT>(
			terminate_event_fd,
#ifndef USE_EVENTFD
            terminate_event_fd_out,
#endif
                grpc::CreateChannel(endpoint,  grpc::InsecureChannelCredentials()),
			chan, model
		);
#undef NON_NULL_STRING
		GRPCSTT::AttachToChannel(grpc_stt);
		try {
            success = grpc_stt->Run(error_status, error_message);
            GRPCSTT::DetachFromChannel(grpc_stt);
		} catch (...) {
			GRPCSTT::DetachFromChannel(grpc_stt);
			throw;
		}
	} catch (const std::exception &ex) {
		error_status = -1;
		error_message = std::string("VoxASRBackground thread finished with exception: ") + ex.what();
	}
	if (!success)
		ast_log(AST_LOG_ERROR, "%s\n", error_message.c_str());
	push_grpcstt_session_finished_event(chan, success, error_status, error_message);
}
