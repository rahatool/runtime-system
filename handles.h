#ifndef HANDLES_H
#define HANDLES_H

#include <map>
#include <memory>
#include <openssl/ssl.h>
#include "v8.h"
#include "uv.h"

// Base class for all C++ objects exposed to JS
struct BaseHandle {
	virtual ~BaseHandle() {}
	virtual void Close() = 0; // Force all handles to be closeable
};

// Represents a `uv_file` (just an int)
struct FileHandle : public BaseHandle {
	uv_file fd;
	std::string path;
	FileHandle(uv_file f, const char* p) : fd(f), path(p) {}
	void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a `uv_tcp_t`
struct NetHandle : public BaseHandle {
	uv_tcp_t handle;
	std::queue<Fiber*> accept_queue;
	AsyncContext* pending_read = nullptr;

	NetHandle() { handle.data = this; }
	void Close() override; // Implemented in primitives_net.cpp
};

// Represents an `SSL*`
struct TLSHandle : public BaseHandle {
	SSL* ssl;
	NetHandle* net_handle; // Underlying TCP handle
	char read_buf[16384];

	TLSHandle(SSL* s, NetHandle* n) : ssl(s), net_handle(n) {}
	~TLSHandle() { SSL_free(ssl); }
	void Close() override; // Implemented in primitives_tls.cpp
};

// Global store for all active handles
class HandleStore {
public:
	static void Init();
	static void Dispose();
	
	static uint64_t Add(std::shared_ptr<BaseHandle> handle);
	static void Remove(uint64_t id);
	
	template<typename T>
	static std::shared_ptr<T> Get(uint64_t id) {
		auto it = store.find(id);
		if (it == store.end()) return nullptr;
		return std::dynamic_pointer_cast<T>(it->second);
	}

private:
	static std::map<uint64_t, std::shared_ptr<BaseHandle>> store;
	static uint64_t next_id;
};

#endif // HANDLES_H