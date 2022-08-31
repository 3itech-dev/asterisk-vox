#ifndef GRPCTTS_CHANNEL_H
#define GRPCTTS_CHANNEL_H

#include "tts_api.grpc.pb.h"
#include "grpctts.h"

#include <string>
#include <memory>


struct grpctts_job_input;

namespace grpc {
class Channel;
};


namespace GRPCTTS {

class Job;
class ChannelBackend;


class Channel
{
public:
	Channel(const char *endpoint);
	~Channel();
	Job *StartJob(double speed, double tone, const std::string &model,
		      const struct grpctts_job_input &job_input);

private:
	std::shared_ptr<ChannelBackend> channel_backend;
};

};

#endif
