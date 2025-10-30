#include "primitives.h"
#include "handles.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <vector>

// --- Handle Implementation ---
void TLSHandle::Close() {
	// SSL_shutdown(ssl); // This should be done in the JS primitive
	// The underlying NetHandle will be closed by its JS wrapper
}

void TLSContextHandle::Close() {
	// No-op, managed by ~TLSContextHandle destructor
}

// --- Internal Helper ---
// This is the core of the async TLS logic.
int PerformSSLOperation(v8::Isolate* isolate, uint64_t net_handle_id, SSL* ssl, int ssl_result) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	
	int err = SSL_get_error(ssl, ssl_result);
	if (err == SSL_ERROR_WANT_READ) {
		// We must poll the underlying socket for readability
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, net_handle_id), v8::Integer::New(isolate, UV_READABLE) };
		v8::Function::New(context, TCP_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	} else if (err == SSL_ERROR_WANT_WRITE) {
		// We must poll the underlying socket for writability
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, net_handle_id), v8::Integer::New(isolate, UV_WRITABLE) };
		v8::Function::New(context, TCP_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	}
	// Success (>= 1), clean shutdown (0), or fatal error (<= 0)
	return ssl_result;
}

// --- SNI Callback ---
// This is the C callback given to OpenSSL.
int OnCertCallback(SSL* ssl, void* arg) {
	// Get the TLSContextHandle that we stored in the SSL_CTX
	TLSContextHandle* context = static_cast<TLSContextHandle*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), 0));
	if (!context || context->cert_resolver.IsEmpty()) {
		return SSL_TLSEXT_ERR_NOACK; // No resolver configured
	}
	
	const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (servername == nullptr) {
		return SSL_TLSEXT_ERR_NOACK; // Fail if no SNI
	}

	v8::Isolate* isolate = context->isolate;
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
	v8::Local<v8::Function> js_resolver = context->cert_resolver.Get(isolate);
	
	v8::Local<v8::Value> args[] = { v8::String::NewFromUtf8(isolate, servername).ToLocalChecked() };
	v8::MaybeLocal<v8::Value> maybe_result;
	
	// Call the JavaScript resolver function
	maybe_result = js_resolver->Call(v8_context, v8::Undefined(isolate), 1, args);

	if (maybe_result.IsEmpty()) {
		return SSL_TLSEXT_ERR_NOACK; // JS function threw an error
	}
	
	v8::Local<v8::Object> cert_obj = maybe_result.ToLocalChecked().As<v8::Object>();
	
	// Get the KeyHandle ID and CertHandle ID from the returned JS object
	uint64_t key_id = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "key").ToLocalChecked()).ToLocalChecked().As<v8::BigInt>()->Uint64Value();
	uint64_t cert_id = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "cert").ToLocalChecked()).ToLocalChecked().As<v8::BigInt>()->Uint64Value();
	
	// Retrieve the C++ objects from the HandleStore
	auto key_handle = HandleStore::Get<KeyHandle>(key_id);
	auto cert_handle = HandleStore::Get<CertHandle>(cert_id);

	if (!key_handle || !cert_handle) {
		return SSL_TLSEXT_ERR_NOACK; // Invalid handles
	}

	// Set the pre-parsed key and cert on the new SSL connection
	if (SSL_use_PrivateKey(ssl, key_handle->pkey) != 1) {
		return SSL_TLSEXT_ERR_NOACK;
	}
	if (SSL_use_certificate(ssl, cert_handle->cert) != 1) {
		return SSL_TLSEXT_ERR_NOACK;
	}
	
	return SSL_TLSEXT_ERR_OK; // Success
}

// --- JS Primitives ---
void TLS_CreateContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Function> resolver = args[0].As<v8::Function>();

	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) { Throw(isolate, "SSL_CTX_new failed"); return; }
	
	// Enable SNI and set our C callback
	SSL_CTX_set_tlsext_servername_callback(ctx, OnCertCallback);
	
	// Create our handle and store the JS resolver function in it
	auto handle = std::make_shared<TLSContextHandle>(isolate, ctx, resolver);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

EVP_PKEY* ParseKey(char* data, size_t len) {
	BIO* bio = BIO_new_mem_buf(data, len);
	EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, NULL, 0, NULL);
	BIO_free(bio);
	return pkey;
}
X509* ParseCert(char* data, size_t len) {
	BIO* bio = BIO_new_mem_buf(data, len);
	X509* cert = PEM_read_bio_X509(bio, NULL, 0, NULL);
	BIO_free(bio);
	return cert;
}

void TLS_ParseKey(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	size_t len;
	char* data = GetUint8ArrayBufferData(args[0], &len);
	EVP_PKEY* pkey = ParseKey(data, len);
	if (!pkey) { Throw(isolate, "Failed to parse private key"); return; }
	auto handle = std::make_shared<KeyHandle>(pkey);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_ParseCert(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	size_t len;
	char* data = GetUint8ArrayBufferData(args[0], &len);
	X509* cert = ParseCert(data, len);
	if (!cert) { Throw(isolate, "Failed to parse certificate"); return; }
	auto handle = std::make_shared<CertHandle>(cert);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t ctx_id = args[0].As<v8::BigInt>()->Uint64Value();
	uint64_t net_handle_id = args[1].As<v8::BigInt>()->Uint64Value();
	
	auto tls_ctx = HandleStore::Get<TLSContextHandle>(ctx_id);
	auto net_handle = HandleStore::Get<NetHandle>(net_handle_id);
	if (!tls_ctx || !net_handle) { Throw(isolate, "Invalid context or socket handle"); return; }

	SSL* ssl = SSL_new(tls_ctx->ctx);
	uv_os_fd_t fd;
	uv_fileno(&net_handle->handle, &fd);
	SSL_set_fd(ssl, fd);
	
	auto sock = std::make_shared<TLSHandle>(ssl, net_handle);

	int r;
	do { r = SSL_accept(ssl); } while (PerformSSLOperation(isolate, net_handle->id, ssl, r) == 0);

	if (r <= 0) {
		SSL_free(ssl); // Don't create a handle
		Throw(isolate, "SSL_accept failed");
		return;
	}
	
	uint64_t id = HandleStore::Add(sock);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t net_handle_id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::String::Utf8Value host(isolate, args[1]);

	auto net_handle = HandleStore::Get<NetHandle>(net_handle_id);
	if (!net_handle) { Throw(isolate, "Invalid socket handle"); return; }

	// Create a new context for each client connection
	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	SSL* ssl = SSL_new(ctx);
	SSL_set_tlsext_host_name(ssl, *host); // Set SNI for client

	uv_os_fd_t fd;
	uv_fileno(&net_handle->handle, &fd);
	SSL_set_fd(ssl, fd);
	
	auto sock = std::make_shared<TLSHandle>(ssl, net_handle);
	
	int r;
	do { r = SSL_connect(ssl); } while (PerformSSLOperation(isolate, net_handle->id, ssl, r) == 0);
	
	if (r <= 0) {
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		Throw(isolate, "SSL_connect failed");
		return;
	}

	uint64_t id = HandleStore::Add(sock);
	SSL_CTX_free(ctx); // Context is only needed for the connect, not long-term
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();

	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }

	size_t len;
	char* data = GetUint8ArrayBufferData(buffer, &len);

	int r;
	do { r = SSL_read(sock->ssl, data, len); } while (PerformSSLOperation(isolate, sock->net_handle->id, sock->ssl, r) == 0);

	if (r <= 0) {
		int err = SSL_get_error(sock->ssl, r);
		if (err == SSL_ERROR_ZERO_RETURN) {
			args.GetReturnValue().Set(v8::BigInt::New(isolate, -1)); // EOF
		} else {
			// Return -1n for general errors too
			args.GetReturnValue().Set(v8::BigInt::New(isolate, -1));
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

	size_t len;
	char* data = GetUint8ArrayBufferData(buffer, &len);

	int r;
	do { r = SSL_write(sock->ssl, data, len); } while (PerformSSLOperation(isolate, sock->net_handle->id, sock->ssl, r) == 0);

	if (r <= 0) { 
		Throw(isolate, "SSL_write failed");
		return;
	}
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)r));
}

void TLS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }
	
	int r;
	do { r = SSL_shutdown(sock->ssl); } while (PerformSSLOperation(isolate, sock->net_handle->id, sock->ssl, r) == 0);
	
	// Let the JS side close the underlying TCP socket
	HandleStore::Remove(id); // This will call ~TLSHandle, which calls SSL_free
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

