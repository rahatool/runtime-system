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
    // Close should initiate async close if needed, but primarily for cleanup.
    // Explicit close methods (e.g., FS_Close) handle the async part & removal.
    virtual void Close() = 0;
};

// Represents a `uv_file` (just an int)
struct FileHandle : public BaseHandle {
    uv_file fd;
    std::string path;
    FileHandle(uv_file f, const char* p) : fd(f), path(p) {}
    void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a directory handle for readdir
struct DirectoryHandle : public BaseHandle {
    uv_dir_t* dir;
    std::string path;
    DirectoryHandle(uv_dir_t* d, const char* p) : dir(d), path(p) {}
    ~DirectoryHandle() { /* uv_closedir happens in Close */ }
    void Close() override; // Implemented in primitives_fs.cpp
};

// Represents a `uv_tcp_t`
struct NetHandle : public BaseHandle {
    uv_tcp_t handle;
    std::queue<Fiber*> accept_queue;
    AsyncContext* pending_read = nullptr; // Only one pending read allowed

    NetHandle() { handle.data = this; }
    void Close() override; // Implemented in primitives_net.cpp
};

// Represents a `uv_udp_t`
struct UDPHandle : public BaseHandle {
    uv_udp_t handle;
    AsyncContext* pending_read = nullptr; // Only one pending read allowed
    UDPHandle() { handle.data = this; }
    void Close() override; // Implemented in primitives_dgram.cpp
};

// Represents an `SSL*`
struct TLSHandle : public BaseHandle {
    SSL* ssl;
    // Store the ID, not the shared_ptr, to avoid circular refs if NetHandle holds TLSHandle
    uint64_t net_handle_id;
    // Keep a weak_ptr for safe access during operations if needed, but primarily use ID
    std::weak_ptr<NetHandle> net_handle_weak;

    TLSHandle(SSL* s, uint64_t net_id, std::shared_ptr<NetHandle> net_ptr)
        : ssl(s), net_handle_id(net_id), net_handle_weak(net_ptr) {}
    ~TLSHandle() { SSL_free(ssl); }
    void Close() override; // Implemented in primitives_tls.cpp
};

// Represents a pre-parsed `EVP_PKEY*`
struct KeyHandle : public BaseHandle {
    EVP_PKEY* pkey;
    KeyHandle(EVP_PKEY* k) : pkey(k) {}
    ~KeyHandle() { EVP_PKEY_free(pkey); }
    void Close() override {} // Dtor handles it
};

// Represents a pre-parsed `X509*`
struct CertHandle : public BaseHandle {
    X509* x509;
    CertHandle(X509* c) : x509(c) {}
    ~CertHandle() { X509_free(x509); }
    void Close() override {} // Dtor handles it
};


// Global store for all active handles
class HandleStore {
public:
    static void Init();
    static void Dispose(); // Cleanup remaining handles on exit

    static uint64_t Add(std::shared_ptr<BaseHandle> handle);
    static void Remove(uint64_t id);

    // Find the ID associated with a shared_ptr (used for TLS -> NET_Poll)
    static uint64_t FindId(std::shared_ptr<BaseHandle> handle);

    template<typename T>
    static std::shared_ptr<T> Get(uint64_t id) {
        auto it = store.find(id);
        if (it == store.end()) return nullptr;
        // Use dynamic_pointer_cast for safe type checking
        return std::dynamic_pointer_cast<T>(it->second);
    }

private:
    static std::map<uint64_t, std::shared_ptr<BaseHandle>> store;
    static uint64_t next_id;
};

#endif // HANDLES_H

