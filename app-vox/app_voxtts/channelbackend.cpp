/*
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

#include "channelbackend.h"

#include "job.h"
#include "roots.pem.h"

#include <unistd.h>
#include <fcntl.h>

#ifdef USE_EVENTFD
#include <sys/eventfd.h>
#include <sys/socket.h>
#else
#include <unistd.h>
#endif

#include <grpcpp/create_channel_posix.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <atomic>

static grpctts_stream_error_callback_t grpctts_stream_error_callback = NULL;

static const std::string grpc_roots_pem_string ((const char *) grpc_roots_pem, sizeof(grpc_roots_pem));


static int *fd_rc_copy(int *p)
{
	__atomic_add_fetch(&p[1], 1, __ATOMIC_SEQ_CST);
	return p;
}
static void fd_rc_destroy(int *p)
{
	if (!p) return;

	int count = __atomic_add_fetch(&p[1], -1, __ATOMIC_SEQ_CST);
	if (count <= 0) {
		close(p[0]);
		free(p);
	}
}
static int fd_rc_cmp(int *p, int *q)
{
	if (*p < *q)
		return -1;
	if (*p > *q)
		return 1;
	return 0;
}

static const grpc_arg_pointer_vtable fd_rc_vtable = {
	.copy = (void *(*)(void *)) fd_rc_copy,
	.destroy = (void (*)(void *)) fd_rc_destroy,
	.cmp = (int (*)(void *, void *)) fd_rc_cmp,
};

static int *fd_rc_new(int fd)
{
	int *fd_rc = (int *) malloc(sizeof(int[2]));
	if (!fd_rc)
		return nullptr;
	fd_rc[0] = fd;
	fd_rc[1] = 1;
	return fd_rc;
}

static void thread_routine(int channel_completion_fd, const std::string &endpoint, const std::string &token, GRPCTTS::ChannelBackend *channel_backend)
{
    grpc::SslCredentialsOptions ssl_credentials_options = {
            .pem_root_certs = grpc_roots_pem_string,
    };
	std::shared_ptr<grpc::Channel> grpc_channel = grpc::CreateChannel(endpoint, token.empty()?grpc::InsecureChannelCredentials():grpc::SslCredentials(ssl_credentials_options));
	channel_backend->SetChannel(grpc_channel );
#ifdef USE_EVENTFD
    eventfd_write(channel_completion_fd, 1);
#else
    write(channel_completion_fd, "1", 1);
#endif

}


namespace GRPCTTS {

void ChannelBackend::SetErrorCallback(grpctts_stream_error_callback_t callback)
{
	grpctts_stream_error_callback = callback;
}

#define NON_NULL_STRING(str) ((str) ? (str) : "")
ChannelBackend::ChannelBackend(const char *endpoint, const char * token) : token(NON_NULL_STRING(token))
#ifdef USE_EVENTFD
    , channel_completion_fd(eventfd(0, EFD_NONBLOCK))
    {
    	thread = std::thread(thread_routine, channel_completion_fd, std::string(endpoint), this);
}
#else
    {
        pipe(channel_completion_fd);
        thread = std::thread(thread_routine, channel_completion_fd[1], std::string(endpoint), this->token, this);
    }
#endif



#undef NON_NULL_STRING
ChannelBackend::~ChannelBackend()
{
	thread.join();
#ifdef USE_EVENTFD
    close(channel_completion_fd);
#else
    close(channel_completion_fd[0]);
    close(channel_completion_fd[1]);
#endif

}
void ChannelBackend::SetChannel(std::shared_ptr<grpc::Channel> grpc_channel)
{
	this->grpc_channel = grpc_channel;
}
std::shared_ptr<grpc::Channel> ChannelBackend::GetChannel()
{
	return grpc_channel;
}
int ChannelBackend::ChannelCompletionFD() const
{
#ifdef USE_EVENTFD
    return channel_completion_fd;
#else
    return channel_completion_fd[0];
#endif
}

std::string ChannelBackend::BuildAuthToken() const {
    if (token.empty())
        return "";
    std::string jwt = "Bearer " + token;

    return jwt;
}


};
