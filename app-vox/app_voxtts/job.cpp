#include "job.h"

#include "bytequeue.h"
#include "channelbackend.h"
#include "grpctts.h"


#include <memory>
#include <thread>
#include <unordered_map>

#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>

#ifdef OLD_PROTO
#include <soxr.h>
#endif

#define CHANNEL_FRAME_SAMPLE_RATE 8000
#define CHANNEL_MAX_OPUS_FRAME_SAMPLES 960
#define CHANNEL_AWAIT_TIMEOUT 60000 /* 60 sec */

#define CXX_STRING(str) (std::string((str) ? (str) : ""))

static grpctts_stream_error_callback_t grpctts_stream_error_callback = NULL;


namespace GRPCTTS {

static void thread_routine(std::shared_ptr<ChannelBackend> channel_backend,
			   double speed, double tone, const std::string &model,
			   const std::string &text, std::shared_ptr<ByteQueue> byte_queue)
{
	int channel_completion_fd = channel_backend->ChannelCompletionFD();
	struct pollfd pfd = {
		.fd = channel_completion_fd,
		.events = POLLIN,
		.revents = 0,
	};
	poll(&pfd, 1, CHANNEL_AWAIT_TIMEOUT);
	if (!(pfd.revents & POLLIN)) {
		if (grpctts_stream_error_callback)
			grpctts_stream_error_callback("GRPC TTS stream finished with error: failed to initialize channel");
		return;
	}

	std::shared_ptr<grpc::Channel> grpc_channel = channel_backend->GetChannel();
	if (!grpc_channel) {
		if (grpctts_stream_error_callback)
			grpctts_stream_error_callback("GRPC TTS stream finished with error: failed to initialize channel");
		return;
	}

	grpc::ClientContext context;
	std::unique_ptr<vox::tts::TTS::Stub> tts_stub = vox::tts::TTS::NewStub(grpc_channel);
    vox::tts::SynthesizeRequest request;
    request.set_text(text);
    if (model.size()) {
        request.set_model(model);
    }
    request.set_speed(speed);
    request.set_tone(tone);
    request.set_sample_rate(CHANNEL_FRAME_SAMPLE_RATE);
    vox::tts::SynthesizeResponse response;
    grpc::Status status = tts_stub->synthesize(&context, request, &response);
    if (status.ok()) {
#ifdef OLD_PROTO
        const char *src=response.audio_content().c_str()+44;
        size_t len = response.audio_content().length()-44;
        if (len>0) {
            char *res = new char[len];
            memset(res,0,len);
            soxr_io_spec_t spec = soxr_io_spec(SOXR_INT16, SOXR_INT16);
            size_t idone, odone;
            soxr_error_t error = soxr_oneshot(22050, 8000, 1, src, len/2, &idone,
                         res, len/2, &odone, &spec, NULL, NULL);
            if (error) {
                printf("SOXR ERROR %s\n", error);

            } else {
                std::string nd(res, odone*2);
                byte_queue->Push(nd);
            }
            delete[] res;
            byte_queue->Terminate(true);
        } else {
            printf("RECEIVED EMPTY RESULT");
            byte_queue->Terminate(true);
        }
#else
        byte_queue->Push(response.audio_content());
        byte_queue->Terminate(true);
#endif
    } else {
        printf("synthesize4\n");

        byte_queue->Terminate(true);
    }
}


void Job::SetErrorCallback(grpctts_stream_error_callback_t callback)
{
	grpctts_stream_error_callback = callback;
}


static std::atomic<int> Job_alloc_balance;

Job::Job(std::shared_ptr<ChannelBackend> channel_backend,
	 double speed, double tone, const std::string &model, const struct grpctts_job_input &job_input)
	: byte_queue(std::make_shared<ByteQueue>())
{
	grpc_init(); // To survie module unloading
	std::thread thread = std::thread(thread_routine,
					 channel_backend,
					 speed, tone, model, CXX_STRING(job_input.text), byte_queue);
	thread.detach();
}
Job::~Job()
{
}
int Job::EventFD()
{
	return byte_queue->EventFD();
}
bool Job::Collect()
{
	return byte_queue->Collect();
}
size_t Job::BufferSize()
{
	return byte_queue->BufferSize();
}
bool Job::TakeBlock(size_t byte_count, void *data)
{
	return byte_queue->TakeBlock(byte_count, data);
}
size_t Job::TakeTail(size_t byte_count, void *data)
{
	return byte_queue->TakeTail(byte_count, data);
}
bool Job::TerminationCalled()
{
	return byte_queue->TerminationCalled();
}
bool Job::CompletionSuccess()
{
	return byte_queue->CompletionSuccess();
}

};
