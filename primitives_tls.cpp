#include "primitives.h"
#include "handles.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <vector>

// --- Handle Implementation ---
void TLSHandle::Close() {
	SSL_shutdown(ssl);
	// The underlying NetHandle will be closed by its own JS wrapper
}

// --- TLS Structs ---
struct TLSContext {
	SSL_CTX* ctx;
	v8::Persistent<v8::Function> cert_resolver;
	v8::Isolate* isolate;

	TLSContext(v8::Isolate* i, SSL_CTX* c, v8::Local<v8::Function> resolver) : ctx(c), isolate(i) {
		cert_resolver.Reset(i, resolver);
		SSL_CTX_set_ex_data(ctx, 0, this);
	}
	~TLSContext() {
		SSL_CTX_free(ctx);
		cert_resolver.Reset();
	}
};

// --- Internal Helper ---
// This is the core of the async TLS logic.
// It performs an SSL operation (like SSL_accept, SSL_read, SSL_write)
// and if it's blocked on I/O (WANT_READ/WANT_WRITE), it yields the fiber
// by calling NET_Poll.
int PerformSSLOperation(TLSSocket* sock, int ssl_result) {
	v8::Isolate* isolate = Fiber::get_current()->isolate();
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	
	int err = SSL_get_error(sock->ssl, ssl_result);
	if (err == SSL_ERROR_WANT_READ) {
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, (uint64_t)HandleStore::Add(sock->net_handle)), v8::Integer::New(isolate, UV_READABLE) };
		v8::Function::New(context, NET_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	} else if (err == SSL_ERROR_WANT_WRITE) {
		v8::Local<v8::Value> poll_args[] = { v8::BigInt::New(isolate, (uint64_t)HandleStore::Add(sock->net_handle)), v8::Integer::New(isolate, UV_WRITABLE) };
		v8::Function::New(context, NET_Poll).ToLocalChecked()->Call(context, v8::Undefined(isolate), 2, poll_args).ToLocalChecked();
		return 0; // Incomplete, retry
	}
	return ssl_result;
}

// --- SNI Callback ---
int OnCertCallback(SSL* ssl, void* arg) {
	TLSContext* context = static_cast<TLSContext*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), 0));
	if (!context || context->cert_resolver.IsEmpty()) return 0; // Fail handshake

	const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (servername == nullptr) return 0; // Fail if no SNI

	v8::Isolate* isolate = context->isolate;
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
	v8::Local<v8::Function> js_resolver = context->cert_resolver.Get(isolate);
	
	v8::Local<v8::Value> args[] = { v8::String::NewFromUtf8(isolate, servername).ToLocalChecked() };
	v8::MaybeLocal<v8::Value> maybe_result = js_resolver->Call(v8_context, v8::Undefined(isolate), 1, args);

	if (maybe_result.IsEmpty()) return 0; // JS function threw
	
	v8::Local<v8::Object> cert_obj = maybe_result.ToLocalChecked().As<v8::Object>();
	v8::Local<v8::String> key_str = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "key").ToLocalChecked()).ToLocalChecked().As<v8::String>();
	v8::Local<v8::String> cert_str = cert_obj->Get(v8_context, v8::String::NewFromUtf8(isolate, "cert").ToLocalChecked()).ToLocalChecked().As<v8::String>();
	
	v8::String::Utf8Value key_utf8(isolate, key_str);
	v8::String::Utf8Value cert_utf8(isolate, cert_str);

	BIO* key_bio = BIO_new_mem_buf(*key_utf8, key_utf8.length());
	BIO* cert_bio = BIO_new_mem_buf(*cert_utf8, cert_utf8.length());
	
	EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, NULL, 0, NULL);
	X509* cert = PEM_read_bio_X509(cert_bio, NULL, 0, NULL);
	
	BIO_free(key_bio);
	BIO_free(cert_bio);

	if (pkey == NULL || cert == NULL) {
		if (pkey) EVP_PKEY_free(pkey);
		if (cert) X509_free(cert);
		return 0; // Fail handshake (bad cert/key)
	}

	SSL_use_PrivateKey(ssl, pkey);
	SSL_use_certificate(ssl, cert);
	
	EVP_PKEY_free(pkey);
	X509_free(cert);
	
	return 1; // Success
}

// --- JS Primitives ---
void TLS_CreateContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Function> resolver = args[0].As<v8::Function>();

	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) { Throw(isolate, "SSL_CTX_new failed"); return; }
	
	SSL_CTX_set_cert_cb(ctx, OnCertCallback, nullptr);
	
	TLSContext* tls_ctx = new TLSContext(isolate, ctx, resolver);
	// Note: This handle isn't "closable" from JS, it's just a context.
	// Maybe it should be a Handle? For now, it's just a pointer.
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (uint64_t)tls_ctx));
}

void TLS_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	TLSContext* tls_ctx = (TLSContext*)args[0].As<v8::BigInt>()->Uint64Value();
	uint64_t net_handle_id = args[1].As<v8::BigInt>()->Uint64Value();
	auto net_handle = HandleStore::Get<NetHandle>(net_handle_id);
	if (!net_handle) { Throw(isolate, "Invalid socket handle"); return; }

	SSL* ssl = SSL_new(tls_ctx->ctx);
	uv_os_fd_t fd;
	uv_fileno(net_handle->handle, &fd);
	SSL_set_fd(ssl, fd);
	
	auto sock = std::make_shared<TLSHandle>(ssl, net_handle.get());

	int r;
	do { r = SSL_accept(ssl); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		Throw(isolate, "SSL_accept failed");
		return;
	}
	
	uint64_t id = HandleStore::Add(sock);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TLS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }

	int r;
	do { r = SSL_read(sock->ssl, sock->read_buf, sizeof(sock->read_buf)); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) {
		int err = SSL_get_error(sock->ssl, r);
		if (err == SSL_ERROR_ZERO_RETURN) {
			args.GetReturnValue().Set(v8::Null(isolate)); // Clean shutdown
		} else {
			Throw(isolate, "SSL_read failed");
		}
		return;
	}
	args.GetReturnValue().Set(v8::String::NewFromUtf8(isolate, sock->read_buf, v8::NewStringType::kNormal, r).ToLocalChecked());
}

void TLS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }
	
	v8::String::Utf8Value data(isolate, args[1]);

	int r;
	do { r = SSL_write(sock->ssl, *data, data.length()); } while (PerformSSLOperation(sock.get(), r) == 0);

	if (r <= 0) { Throw(isolate, "SSL_write failed"); return; }
	args.GetReturnValue().Set(v8::Integer::New(isolate, r));
}

void TLS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto sock = HandleStore::Get<TLSHandle>(id);
	if (!sock) { Throw(isolate, "Invalid TLS socket handle"); return; }
	
	int r;
	do { r = SSL_shutdown(sock->ssl); } while (PerformSSLOperation(sock.get(), r) == 0);
	
	// Now close the underlying TCP socket
	sock->net_handle->Close();
	HandleStore::Remove(id);
}

void InitializeTLS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tls_obj = v8::Object::New(isolate);
	SET_METHOD(tls_obj, "createContext", TLS_CreateContext);
	SET_METHOD(tls_obj, "accept", TLS_Accept);
	SET_METHOD(tls_obj, "read", TLS_Read);
	SET_METHOD(tls_obj, "write", TLS_Write);
	SET_METHOD(tls_obj, "close", TLS_Close);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tls").ToLocalChecked(), tls_obj).Check();
}