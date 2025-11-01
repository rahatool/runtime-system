#ifndef HANDLES_H
#define HANDLES_H

#include <map>
#include <memory>
#include <queue>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include "v8.h"
#include "uv.h"
#include "fiber.h" // For Fiber

// Forward declaration to avoid include cycle
struct AsyncContext;

// Base class for all C++ objects exposed to JS
struct BaseHandle {
	virtual ~BaseHandle() {}
	virtual void Close() = 0; // Force all handles to be closeable
	uint64_t id; // Store its own ID for removal
};

// Represents a `uv_file` (just an int)
struct FileHandle : public BaseHandle {
	uv_file fd;
	std::string path;
	FileHandle(uv_file f, const char* p) : fd(f), path(p) {}
	void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a `uv_dir_t*` (scandir result)
struct DirectoryHandle : public BaseHandle {
	std::vector<uv_dirent_t> entries;
	size_t index;
	DirectoryHandle(std::vector<uv_dirent_t> e) : entries(std::move(e)), index(0) {}
	void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a `uv_tcp_t`
struct NetHandle : public BaseHandle {
	uv_tcp_t handle;
	std::queue<Fiber*> accept_queue;
	AsyncContext* pending_read = nullptr;

	NetHandle() { handle.data = this; }
	void Close() override; // Implemented in primitives_tcp.cpp
};

// Represents a `uv_udp_t`
struct UDPHandle : public BaseHandle {
	uv_udp_t handle;
	AsyncContext* pending_read = nullptr;
	UDPHandle() { handle.data = this; }
	void Close() override; // Implemented in primitives_udp.cpp
};

// Represents a TLS Context `SSL_CTX*`
struct TLSContextHandle : public BaseHandle {
	SSL_CTX* ctx;
	v8::Persistent<v8::Function> cert_resolver;
	v8::Isolate* isolate;

	TLSContextHandle(v8::Isolate* i, SSL_CTX* c, v8::Local<v8::Function> resolver) : ctx(c), isolate(i) {
		cert_resolver.Reset(i, resolver);
		SSL_CTX_set_ex_data(ctx, 0, this);
	}
	~TLSContextHandle() {
		SSL_CTX_free(ctx);
		cert_resolver.Reset();
	}
	void Close() override { /* No-op, managed by ~TLSContextHandle */ }
};

// Represents an `SSL*`
struct TLSHandle : public BaseHandle {
	SSL* ssl;
	std::shared_ptr<NetHandle> net_handle; // Underlying TCP handle

	TLSHandle(SSL* s, std::shared_ptr<NetHandle> n) : ssl(s), net_handle(n) {}
	~TLSHandle() { SSL_free(ssl); }
	void Close() override; // Implemented in primitives_tls.cpp
};

// Represents a parsed `EVP_PKEY*`
struct KeyHandle : public BaseHandle {
	EVP_PKEY* pkey;
	KeyHandle(EVP_PKEY* k) : pkey(k) {}
	~KeyHandle() { EVP_PKEY_free(pkey); }
	void Close() override { /* No-op, managed by ~KeyHandle */ }
};

// Represents a parsed `X509*`
struct CertHandle : public BaseHandle {
	X509* cert;
	CertHandle(X509* c) : cert(c) {}
	~CertHandle() { X509_free(cert); }
	void Close() override { /* No-op, managed by ~CertHandle */ }
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

