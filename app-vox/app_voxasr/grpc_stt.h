
#ifndef GRPC_STT_H
#define GRPC_STT_H

#ifdef __cplusplus
extern "C" {
#endif

struct ast_channel;

enum grpc_stt_frame_format {
	GRPC_STT_FRAME_FORMAT_ALAW = 0,
	GRPC_STT_FRAME_FORMAT_MULAW = 1,
	GRPC_STT_FRAME_FORMAT_SLINEAR16 = 2,
};

extern void grpc_stt_run(
	int terminate_event_fd,
#ifndef USE_EVENTFD
    int terminate_event_fd_out,
#endif
    const char *endpoint,
    const char *token,
	struct ast_channel *chan,
	const char *model);

#ifdef __cplusplus
};
#endif

#endif
