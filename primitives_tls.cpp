#include "primitives.h"
#include "handles.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <vector>

// --- Handle Implementation ---
TLSContextHandle::TLSContextHandle(v8::Isolate* i, SSL_CTX* c, v8::Local<v8::Function> resolver) 
	: isolate(i), ctx(c) {
	if (!resolver->IsNull()) {
		cert_resolver.Reset(i, resolver);
		SSL_CTX_set_ex_data(ctx, 0, this); // Store pointer for callback
	}
}
TLSContextHandle::~TLSContextHandle() {
	SSL_CTX_free(ctx);
	cert_resolver.Reset();
}
void TLSHandle::Close() {
	// Attempt graceful shutdown, ignoring errors for close
	if (ssl) {
		SSL_shutdown(ssl); 
		// SSL_free happens in ~TLSHandle
	}
	// Do NOT close the underlying NetHandle here, it has its own lifecycle
}

// --- Internal Helper ---
int PerformSSLOperation(TLSHandle* sock, int ssl_result) {
	v8::Isolate* isolate = Fiber::get_current()->isolate();
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	
	int err = SSL_get_error(sock->ssl, ssl_result);
	if (err == SSL_ERROR_WANT_READ) {
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, sock->net_handle_id), v8::Integer::New(isolate, UV_READABLE) };
		v8::Function::New(context, TCP_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	} else if (err == SSL_ERROR_WANT_WRITE) {
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, sock->net_handle_id), v8::Integer::New(isolate, UV_WRITABLE) };
		v8::Function::New(context, TCP_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	}
	return ssl_result;
}

// --- SNI Callback ---
int OnCertCallback(SSL* ssl, void* arg) {
	TLSContextHandle* context = static_cast<TLSContextHandle*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), 0));
	// Check moved to TLS_CreateContext

	const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (servername == nullptr) return SSL_TLSEXT_ERR_NOACK; // Fail if no SNI

	v8::Isolate* isolate = context->isolate;
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
	v8::Local<v8::Function> js_resolver = context->cert_resolver.Get(isolate);
	
	v8::Local<v8::Value> args[] = { v8::String::NewFromUtf8(isolate, servername).ToLocalChecked() };
	v8::MaybeLocal<v8::Value> maybe_result = js_resolver->Call(v8_context, v8::Undefined(isolate), 1, args);

	if (maybe_result.IsEmpty()) return SSL_TLSEXT_ERR_ALERT_FATAL; // JS function threw
	
	v8::Local<v8::Object> cert_obj = maybe_result.ToLocalChecked().As<v8::Object>();
	uint64_t key_id = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "key").ToLocalChecked()).ToLocalChecked().As<v8::BigInt>()->Uint64Value();
	uint64_t cert_id = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "cert").ToLocalChecked()).ToLocalChecked().As<v8::BigInt>()->Uint64Value();

	auto key_handle = HandleStore::Get<KeyHandle>(key_id);
	auto cert_handle = HandleStore::Get<CertHandle>(cert_id);

	if (!key_handle || !cert_handle) return SSL_TLSEXT_ERR_ALERT_FATAL; // Bad handle IDs

	// Use SSL_CTX_use_PrivateKey and SSL_CTX_use_certificate if you need to set them per-context
	// For SNI, use the SSL object directly
	if (SSL_use_PrivateKey(ssl, key_handle->pkey) != 1) return SSL_TLSEXT_ERR_ALERT_FATAL;
	if (SSL_use_certificate(ssl, cert_handle->cert) != 1) return SSL_TLSEXT_ERR_ALERT_FATAL;
	
	return SSL_TLSEXT_ERR_OK; // Success
}

// --- JS Primitives ---
void TLS_CreateContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Function> resolver; // Optional
	bool is_client = false;
	
	if (args[0]->IsBoolean()) { // Client context
		is_client = args[0]->IsTrue();
	} else if (args[0]->IsFunction()) { // Server context with resolver
		resolver = args[0].As<v8::Function>();
	} else {
		Throw(isolate, "First argument must be a boolean (for client) or a function (for server resolver)");
		return;
	}

	SSL_CTX* ctx = SSL_CTX_new(is_client ? TLS_client_method() : TLS_server_method());
	if (!ctx) { Throw(isolate, "SSL_CTX_new failed"); return; }
	
	if (!is_client && !resolver->IsNull()) {
		// Set SNI callback only for server with resolver
		SSL_CTX_set_cert_cb(ctx, OnCertCallback, nullptr);
	} else if (!is_client) {
		SSL_CTX_free(ctx);
		Throw(isolate, "Server context requires a certificateResolver function");
		return;
	}
	
	auto handle = std::make_shared<TLSContextHandle>(isolate, ctx, resolver);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

EVP_PKEY* ParsePrivateKey(char* data, size_t len) {
	BIO* bio = BIO_new_mem_buf(data, len);
	if (!bio) return nullptr;
	EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, 0, NULL);
	BIO_free(bio);
	return pkey;
}
X509* ParseCertificate(char* data, size_t len) {
	BIO* bio = BIO_new_mem_buf(data, len);
	if (!bio) return nullptr;
	X509* cert = PEM_read_bio_X509(bio, NULL, 0, NULL);
	BIO_free(bio);
	return cert;
}

void TLS_ParseKey(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> buffer = args[0].As<v8::Uint8Array>();
	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);
	
	EVP_PKEY* pkey = ParsePrivateKey(data, len);
	if (!pkey) { Throw(isolate, "Failed to parse private key"); return; }
	
	auto handle = std::make_shared<KeyHandle>(pkey);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_ParseCert(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> buffer = args[0].As<v8::Uint8Array>();
	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);
	
	X509* cert = ParseCertificate(data, len);
	if (!cert) { Throw(isolate, "Failed to parse certificate"); return; }
	
	auto handle = std::make_shared<CertHandle>(cert);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t ctx_id = args[0].As<v8::BigInt>()->Uint64Value();
	uint64_t net_id = args[1].As<v8::BigInt>()->Uint64Value();
	
	auto tls_ctx_handle = HandleStore::Get<TLSContextHandle>(ctx_id);
	auto net_handle = HandleStore::Get<NetHandle>(net_id);
	if (!tls_ctx_handle || !net_handle) { Throw(isolate, "Invalid handle"); return; }

	SSL* ssl = SSL_new(tls_ctx_handle->ctx);
	uv_os_fd_t fd;
	uv_fileno(&net_handle->handle, &fd);
	SSL_set_fd(ssl, fd);
	
	auto sock = std::make_shared<TLSHandle>(ssl, net_id);

	int r;
	do { r = SSL_accept(ssl); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		Throw(isolate, "SSL_accept failed");
		return;
	}
	
	uint64_t id = HandleStore::Add(sock);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t net_id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::String::Utf8Value hostname(isolate, args[1]);
	
	auto net_handle = HandleStore::Get<NetHandle>(net_id);
	if (!net_handle) { Throw(isolate, "Invalid socket handle"); return; }

	// Create client context on the fly
	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx) { Throw(isolate, "SSL_CTX_new failed"); return; }
	SSL* ssl = SSL_new(ctx);
	SSL_CTX_free(ctx); // SSL_new increments refcount

	SSL_set_servername(ssl, TLSEXT_NAMETYPE_host_name, *hostname);
	uv_os_fd_t fd;
	uv_fileno(&net_handle->handle, &fd);
	SSL_set_fd(ssl, fd);

	auto sock = std::make_shared<TLSHandle>(ssl, net_id);
	
	int r;
	do { r = SSL_connect(ssl); } while (PerformSSLOperation(sock.get(), r) == 0);
	
	if (r <= 0) {
		Throw(isolate, "SSL_connect failed");
		return;
	}
	
	uint64_t id = HandleStore::Add(sock);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}


void TLS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }
	
	char* js_buf = GetUint8ArrayBufferData(buffer);
	size_t js_buf_len = GetUint8ArrayByteLength(buffer);

	int r;
	do { r = SSL_read(sock->ssl, js_buf, js_buf_len); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		int err = SSL_get_error(sock->ssl, r);
		if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL) { // Treat syscall error (like connection reset) as EOF
			args.GetReturnValue().Set(v8::Null(isolate)); // Clean shutdown or abrupt close
		} else {
			Throw(isolate, "SSL_read failed");
		}
		return;
	}
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)r));
}

void TLS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }
	
	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);

	int r;
	do { r = SSL_write(sock->ssl, data, len); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) { Throw(isolate, "SSL_write failed"); return; }
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)r));
}

void TLS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) return; // Already closed or invalid

	// Attempt graceful shutdown, ignoring WANT_READ/WANT_WRITE here for close
	SSL_shutdown(sock->ssl); 

	// Remove the TLS handle, letting GC handle the underlying TCP one
	HandleStore::Remove(id); 
}


void InitializeTLS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tls_obj = v8::Object::New(isolate);
	SET_METHOD(tls_obj, "createContext", TLS_CreateContext);
	SET_METHOD(tls_obj, "parseKey", TLS_ParseKey);
	SET_METHOD(tls_obj, "parseCert", TLS_ParseCert);
	SET_METHOD(tls_obj, "accept", TLS_Accept);
	SET_METHOD(tls_obj, "connect", TLS_Connect);
	SET_METHOD(tls_obj, "read", TLS_Read);
	SET_METHOD(tls_obj, "write", TLS_Write);
	SET_METHOD(tls_obj, "close", TLS_Close);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tls").ToLocalChecked(), tls_obj).Check();
}

