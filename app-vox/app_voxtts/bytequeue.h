#ifndef GRPCTTS_BYTE_QUEUE_H
#define GRPCTTS_BYTE_QUEUE_H

#include <string>
#include <deque>


namespace GRPCTTS {

struct Record;

// Single sender + single reciever byte queue
class ByteQueue
{
public:
	ByteQueue();
	~ByteQueue();

	// Sender-only methods
	void Push(const std::string &data);
	void Terminate(bool success);

	// Reader-only methods
	int EventFD();
	bool TerminationCalled();
	bool CompletionSuccess();
	bool Collect();
	size_t BufferSize();
	bool TakeBlock(size_t byte_count, void *data);
	size_t TakeTail(size_t byte_count, void *data);

private:
	void PushRecord(Record *object);
	void ExtractBytes(size_t byte_count, void *data);
	void StoreReverseList(Record *queue);

private:
#ifdef USE_EVENTFD
        int event_fd;
#else
        int event_fd[2];
#endif

	Record *exchange_head;
	Record *recieved_head;
	Record **recieved_tail_p;
	size_t head_offset;
	size_t recieved_byte_count;
	bool termination_pushed;
	bool termination_called;
	bool completion_success;
};

};

#endif
