#ifndef HANDLES_H
#define HANDLES_H

#include <map>
#include <memory>
#include <queue>
#include <openssl/ssl.h>
#include "v8.h"
#include "uv.h"
#include "primitives.h" // For AsyncContext

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

// Represents a `uv_dir_t` pointer for directory iteration
struct DirHandle : public BaseHandle {
	uv_dir_t* dir;
	std::string path;
	DirHandle(uv_dir_t* d, const char* p) : dir(d), path(p) {}
	void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a `uv_tcp_t`
struct NetHandle : public BaseHandle {
	uv_tcp_t handle;
	std::queue<Fiber*> accept_queue;
	AsyncContext* pending_read = nullptr;

	NetHandle() { handle.data = this; }
	~NetHandle(); // Implemented in primitives_tcp.cpp
	void Close() override; // Implemented in primitives_tcp.cpp
};

// Represents a `uv_udp_t`
struct UDPHandle : public BaseHandle {
	uv_udp_t handle;
	AsyncContext* pending_read = nullptr;
	
	UDPHandle() { handle.data = this; }
	~UDPHandle(); // Implemented in primitives_udp.cpp
	void Close() override; // Implemented in primitives_udp.cpp
};

// Represents a pre-parsed `EVP_PKEY*`
struct KeyHandle : public BaseHandle {
	EVP_PKEY* pkey;
	KeyHandle(EVP_PKEY* k) : pkey(k) {}
	~KeyHandle() { EVP_PKEY_free(pkey); }
	void Close() override {} // No-op, dtor handles it
};

// Represents a pre-parsed `X509*`
struct CertHandle : public BaseHandle {
	X509* cert;
	CertHandle(X509* c) : cert(c) {}
	~CertHandle() { X509_free(cert); }
	void Close() override {} // No-op, dtor handles it
};

// Represents an `SSL_CTX*`
struct TLSContextHandle : public BaseHandle {
	SSL_CTX* ctx;
	v8::Persistent<v8::Function> cert_resolver;
	v8::Isolate* isolate;

	TLSContextHandle(v8::Isolate* i, SSL_CTX* c, v8::Local<v8::Function> resolver);
	~TLSContextHandle();
	void Close() override {} // No-op, dtor handles it
};

// Represents an `SSL*`
struct TLSHandle : public BaseHandle {
	SSL* ssl;
	uint64_t net_handle_id; // ID of the underlying TCP handle

	TLSHandle(SSL* s, uint64_t n_id) : ssl(s), net_handle_id(n_id) {}
	~TLSHandle() { SSL_free(ssl); }
	void Close() override; // Implemented in primitives_tls.cpp
};

// Global store for all active handles
class HandleStore {
public:
	static void Init();
	static void Dispose();
	
	static uint64_t Add(std::shared_ptr<BaseHandle> handle);
	static bool Remove(uint64_t id); // Returns true on success
	
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

