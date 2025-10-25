#include "primitives.h"
#include "handles.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <vector>

// --- Handle Implementation ---
void TLSHandle::Close() {
	SSL_shutdown(ssl);
	// Underlying NetHandle is closed separately by its JS owner
}

// --- TLS Structs ---
// TLSContext now managed via HandleStore (as a BaseHandle variant if needed, or raw ptr cleanup)
// Let's make it simpler: Manage the raw pointer via a specific free function.
struct TLSContext {
	SSL_CTX* ctx;
	v8::Persistent<v8::Function> cert_resolver;
	v8::Isolate* isolate;

	TLSContext(v8::Isolate* i, SSL_CTX* c, v8::Local<v8::Function> resolver) : ctx(c), isolate(i) {
		cert_resolver.Reset(i, resolver);
		SSL_CTX_set_ex_data(ctx, 0, this); // Store this context pointer
	}
	~TLSContext() {
		// std::cout << "Freeing TLSContext" << std::endl; // Debugging
		SSL_CTX_free(ctx);
		cert_resolver.Reset();
	}
};


// --- Internal Helper ---
int PerformSSLOperation(TLSHandle* sock, int ssl_result) {
	v8::Isolate* isolate = Fiber::get_current()->isolate();
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::TryCatch try_catch(isolate);

	int err = SSL_get_error(sock->ssl, ssl_result);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
		uint64_t net_handle_id = sock->net_handle_id;
		if (net_handle_id == 0) return SSL_ERROR_SYSCALL;

		int events = (err == SSL_ERROR_WANT_READ) ? UV_READABLE : UV_WRITABLE;
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, net_handle_id), v8::Integer::New(isolate, events) };

		v8::Local<v8::Object> primordials = context->Global()->Get(context, v8::String::NewFromUtf8(isolate, "__primordials").ToLocalChecked()).ToLocalChecked().As<v8::Object>();
		v8::Local<v8::Object> tcp_obj = primordials->Get(context, v8::String::NewFromUtf8(isolate, "tcp").ToLocalChecked()).ToLocalChecked().As<v8::Object>();
		v8::Local<v8::Function> net_poll_func = tcp_obj->Get(context, v8::String::NewFromUtf8(isolate, "poll").ToLocalChecked()).ToLocalChecked().As<v8::Function>();

		net_poll_func->Call(context, v8::Undefined(isolate), 2, poll_args);

		if (try_catch.HasCaught()) {
			 // If poll threw (e.g., bad handle), propagate error back to SSL layer
			 // This might not be the *best* way, but alerts the caller.
			 return SSL_ERROR_SYSCALL;
		}
		return 0; // Incomplete, retry
	}
	return ssl_result;
}

// --- SNI Callback ---
int OnCertCallback(SSL* ssl, void* arg) {
	TLSContext* context = static_cast<TLSContext*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), 0));
	// The check for resolver existence is now in TLS_CreateContext

	const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (servername == nullptr) return SSL_TLSEXT_ERR_NOACK; // No SNI sent by client

	v8::Isolate* isolate = context->isolate;
	// --- IMPORTANT ---
	// This callback can be invoked from a libuv thread pool thread during async handshake.
	// We MUST acquire the V8 locker to safely interact with JavaScript.
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolate_scope(isolate);
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
	v8::Context::Scope context_scope(v8_context);
	v8::TryCatch try_catch(isolate); // Catch JS exceptions

	v8::Local<v8::Function> js_resolver = context->cert_resolver.Get(isolate);
	v8::Local<v8::Value> js_args[] = { v8::String::NewFromUtf8(isolate, servername).ToLocalChecked() };
	v8::MaybeLocal<v8::Value> maybe_result = js_resolver->Call(v8_context, v8::Undefined(isolate), 1, js_args);

	// Check for JS exception or invalid return IMMEDIATELY
	if (maybe_result.IsEmpty() || try_catch.HasCaught()) {
		ReportException(isolate, &try_catch);
		return SSL_TLSEXT_ERR_ALERT_FATAL; // Tell OpenSSL to fail handshake
	}
	v8::Local<v8::Value> result_val = maybe_result.ToLocalChecked();
	if (!result_val->IsObject()) return SSL_TLSEXT_ERR_ALERT_FATAL;

	v8::Local<v8::Object> cert_obj = result_val.As<v8::Object>();
	v8::MaybeLocal<v8::Value> key_val_maybe = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "key").ToLocalChecked());
	v8::MaybeLocal<v8::Value> cert_val_maybe = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "cert").ToLocalChecked());

	if (key_val_maybe.IsEmpty() || cert_val_maybe.IsEmpty()) return SSL_TLSEXT_ERR_ALERT_FATAL;
	v8::Local<v8::Value> key_val = key_val_maybe.ToLocalChecked();
	v8::Local<v8::Value> cert_val = cert_val_maybe.ToLocalChecked();
	if (!key_val->IsBigInt() || !cert_val->IsBigInt()) return SSL_TLSEXT_ERR_ALERT_FATAL;

	uint64_t key_id = key_val.As<v8::BigInt>()->Uint64Value();
	uint64_t cert_id = cert_val.As<v8::BigInt>()->Uint64Value();

	// Use shared_ptr temporarily within this scope
	auto key_handle = HandleStore::Get<KeyHandle>(key_id);
	auto cert_handle = HandleStore::Get<CertHandle>(cert_id);
	if (!key_handle || !cert_handle) return SSL_TLSEXT_ERR_ALERT_FATAL;

	// Use the raw pointers with OpenSSL
	if (SSL_use_PrivateKey(ssl, key_handle->pkey) <= 0) return SSL_TLSEXT_ERR_ALERT_FATAL;
	if (SSL_use_certificate(ssl, cert_handle->x509) <= 0) return SSL_TLSEXT_ERR_ALERT_FATAL;

	return SSL_TLSEXT_ERR_OK; // Success
}

// --- JS Primitives ---
void TLS_ParseKey(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> key_buf = args[0].As<v8::Uint8Array>();
	char* key_data; size_t key_len;
	if (!GetUint8ArrayData(key_buf, &key_data, &key_len)) return;

	BIO* key_bio = BIO_new_mem_buf(key_data, key_len);
	if (!key_bio) { Throw(isolate, "BIO_new_mem_buf failed"); return; }
	EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, NULL, 0, NULL);
	BIO_free(key_bio);
	if (pkey == NULL) { Throw(isolate, "Failed to parse private key"); return; }

	auto handle = std::make_shared<KeyHandle>(pkey);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_ParseCert(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> cert_buf = args[0].As<v8::Uint8Array>();
	char* cert_data; size_t cert_len;
	if (!GetUint8ArrayData(cert_buf, &cert_data, &cert_len)) return;

	BIO* cert_bio = BIO_new_mem_buf(cert_data, cert_len);
	if (!cert_bio) { Throw(isolate, "BIO_new_mem_buf failed"); return; }
	X509* cert = PEM_read_bio_X509(cert_bio, NULL, 0, NULL);
	BIO_free(cert_bio);
	if (cert == NULL) { Throw(isolate, "Failed to parse certificate"); return; }

	auto handle = std::make_shared<CertHandle>(cert);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_CreateContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Value> resolver_val = args[0];
	if (resolver_val->IsNullOrUndefined() || !resolver_val->IsFunction()) {
		Throw(isolate, "TLS_CreateContext requires a certificateResolver function"); return;
	}
	v8::Local<v8::Function> resolver = resolver_val.As<v8::Function>();

	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) { Throw(isolate, "SSL_CTX_new failed"); return; }
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);
	SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!MD5:!RC4:!eNULL");
	SSL_CTX_set_cert_cb(ctx, OnCertCallback, nullptr);

	TLSContext* tls_ctx = new TLSContext(isolate, ctx, resolver);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (uint64_t)tls_ctx));
}

// New primitive to free the context
void TLS_FreeContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t ptr_val = args[0].As<v8::BigInt>()->Uint64Value();
	TLSContext* tls_ctx = reinterpret_cast<TLSContext*>(ptr_val);
	delete tls_ctx; // Destructor handles SSL_CTX_free and persistent handle reset
}

void TLS_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	TLSContext* tls_ctx = (TLSContext*)args[0].As<v8::BigInt>()->Uint64Value();
	uint64_t net_handle_id = args[1].As<v8::BigInt>()->Uint64Value();
	auto net_handle_ptr = HandleStore::Get<NetHandle>(net_handle_id);
	if (!net_handle_ptr) { Throw(isolate, "Invalid socket handle for TLS accept"); return; }

	SSL* ssl = SSL_new(tls_ctx->ctx);
	if (!ssl) { Throw(isolate, "SSL_new failed"); return; }
	uv_os_fd_t fd;
	if (uv_fileno(&net_handle_ptr->handle, &fd) != 0) { SSL_free(ssl); Throw(isolate, "uv_fileno failed"); return; }
	if (SSL_set_fd(ssl, fd) == 0) { SSL_free(ssl); Throw(isolate, "SSL_set_fd failed"); return; }

	auto sock = std::make_shared<TLSHandle>(ssl, net_handle_id, net_handle_ptr);
	int r;
	do { r = SSL_accept(ssl); } while (PerformSSLOperation(sock.get(), r) == 0);
	if (r <= 0) {
		// Handshake failed - clean up SSL*, but keep NetHandle alive
		char err_buf[256]; ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
		std::string msg = "SSL_accept failed: " + std::string(err_buf); Throw(isolate, msg.c_str());
		// Do not add sock to HandleStore
		return;
	}

	uint64_t id = HandleStore::Add(sock);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) {
	 v8::Isolate* isolate = args.GetIsolate();
	 uint64_t net_handle_id = args[0].As<v8::BigInt>()->Uint64Value();
	 v8::String::Utf8Value hostname(isolate, args[1]); // For SNI
	 auto net_handle_ptr = HandleStore::Get<NetHandle>(net_handle_id);
	 if (!net_handle_ptr) { Throw(isolate, "Invalid socket handle for TLS connect"); return; }

	 SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	 if (!ctx) { Throw(isolate, "SSL_CTX_new (client) failed"); return; }
	 // Add verification options here...

	 SSL* ssl = SSL_new(ctx);
	 if (!ssl) { SSL_CTX_free(ctx); Throw(isolate, "SSL_new (client) failed"); return; }
	 SSL_set_tlsext_host_name(ssl, *hostname);

	 uv_os_fd_t fd;
	 if (uv_fileno(&net_handle_ptr->handle, &fd) != 0) { SSL_free(ssl); SSL_CTX_free(ctx); Throw(isolate, "uv_fileno failed"); return; }
	 if (SSL_set_fd(ssl, fd) == 0) { SSL_free(ssl); SSL_CTX_free(ctx); Throw(isolate, "SSL_set_fd failed"); return; }

	 auto sock = std::make_shared<TLSHandle>(ssl, net_handle_id, net_handle_ptr);
	 // Store the CTX with the SSL object for client connections
	 SSL_set_SSL_CTX(ssl, ctx); // So it gets freed when SSL* is freed

	 int r;
	 do { r = SSL_connect(ssl); } while (PerformSSLOperation(sock.get(), r) == 0);
	 if (r <= 0) {
		// Handshake failed
		char err_buf[256]; ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
		std::string msg = "SSL_connect failed: " + std::string(err_buf); Throw(isolate, msg.c_str());
		return;
	 }

	 uint64_t id = HandleStore::Add(sock);
	 args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }

	char* data_ptr; size_t length;
	if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return;

	int r;
	do { r = SSL_read(sock->ssl, data_ptr, length); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		int err = SSL_get_error(sock->ssl, r);
		if (err == SSL_ERROR_ZERO_RETURN) { args.GetReturnValue().Set(v8::Null(isolate)); } // EOF
		else {
			char err_buf[256]; ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
			std::string msg = "SSL_read failed: " + std::string(err_buf); Throw(isolate, msg.c_str());
		}
		return;
	}
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)r)); // bytesRead
}

void TLS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }

	char* data_ptr; size_t length;
	if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return;

	int r;
	do { r = SSL_write(sock->ssl, data_ptr, length); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		char err_buf[256]; ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
		std::string msg = "SSL_write failed: " + std::string(err_buf); Throw(isolate, msg.c_str());
		return;
	}
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)r)); // bytesWritten
}

void TLS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }

	// Initiate SSL shutdown (non-blocking)
	sock->Close();
	// Do not remove from HandleStore here; FinalizationRegistry calls HANDLES_Free
	// The underlying TCP socket is managed independently.
}

void InitializeTLS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tls_obj = v8::Object::New(isolate);
	SET_METHOD(tls_obj, "parseKey", TLS_ParseKey);
	SET_METHOD(tls_obj, "parseCert", TLS_ParseCert);
	SET_METHOD(tls_obj, "createContext", TLS_CreateContext);
	SET_METHOD(tls_obj, "freeContext", TLS_FreeContext); // Added
	SET_METHOD(tls_obj, "accept", TLS_Accept);
	SET_METHOD(tls_obj, "connect", TLS_Connect);
	SET_METHOD(tls_obj, "read", TLS_Read);
	SET_METHOD(tls_obj, "write", TLS_Write);
	SET_METHOD(tls_obj, "close", TLS_Close);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tls").ToLocalChecked(), tls_obj).Check();
}

