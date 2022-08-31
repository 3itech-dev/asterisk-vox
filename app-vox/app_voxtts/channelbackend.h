
#ifndef GRPCTTS_CHANNEL_BACKEND_H
#define GRPCTTS_CHANNEL_BACKEND_H

#include <memory>
#include <thread>


typedef void (*grpctts_stream_error_callback_t)(const char *message);


struct grpctts_job_input;

namespace grpc {
class Channel;
};


namespace GRPCTTS {

class ChannelBackend
{
public:
	static void SetErrorCallback(grpctts_stream_error_callback_t callback);

public:
	ChannelBackend(const char *endpoint);
	~ChannelBackend();
	void SetChannel(std::shared_ptr<grpc::Channel> grpc_channel);
	std::shared_ptr<grpc::Channel> GetChannel(); // To be called after polling on channel_completion_fd shows some data
	int ChannelCompletionFD() const;

private:
	std::shared_ptr<grpc::Channel> grpc_channel;
#ifdef USE_EVENTFD
    int channel_completion_fd;
#else
    int channel_completion_fd[2];
#endif

	std::thread thread;
};

};

#endif
