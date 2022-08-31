#ifndef GRPCTTS_JOB_H
#define GRPCTTS_JOB_H

#include "tts_api.grpc.pb.h"
#include "grpctts.h"

#include <string>
#include <memory>


typedef void (*grpctts_stream_error_callback_t)(const char *message);

struct grpctts_job_input;

namespace grpc {
class Channel;
};


namespace GRPCTTS {

class ByteQueue;
class ChannelBackend;


class Job
{
public:
	static void SetErrorCallback(grpctts_stream_error_callback_t callback);

public:
	Job(std::shared_ptr<ChannelBackend> channel_backend,
	    double speed, double tone, const std::string &model,
	    const struct grpctts_job_input &job_input);
	~Job();
	int EventFD();
	bool Collect();
	size_t BufferSize();
	bool TakeBlock(size_t byte_count, void *data);
	size_t TakeTail(size_t byte_count, void *data);
	bool TerminationCalled();
	bool CompletionSuccess();

private:
	std::shared_ptr<ByteQueue> byte_queue;
};

};

#endif
