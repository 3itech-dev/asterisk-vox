#include "channel.h"

#include "job.h"
#include "channelbackend.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/create_channel_posix.h>
#include <grpcpp/security/credentials.h>


namespace GRPCTTS {

Channel::Channel(const char *endpoint)
	: channel_backend (std::make_shared<ChannelBackend> (endpoint))
{
}
Channel::~Channel()
{
}
Job *Channel::StartJob(double speed, double tone, const std::string &model,  const struct grpctts_job_input &job_input)
{
	return new Job(channel_backend,
		       speed, tone, model, job_input);
}

};
