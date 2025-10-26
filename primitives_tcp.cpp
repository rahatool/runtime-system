#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>
#include <vector>

// --- Handle Implementation ---
void NetHandle::Close() {
	if (handle && !uv_is_closing(handle)) {
		uv_close(handle, [](uv_handle_t* h){
			// Destructor will free handle memory via shared_ptr
		});
	}
}
NetHandle::~NetHandle() {
	// If uv_close wasn't called (e.g. GC), try to close here,
	// but it might leak if loop isn't running. Best practice is explicit close().
	if (handle && !uv_is_closing(handle)) {
		uv_close(handle, [](uv_handle_t* h){
			delete (uv_tcp_t*)h;
		});
	} else if (!handle) {
		// Handle might already be deleted if uv_close callback ran
	} else {
		// uv_close already called, uv loop will free it.
	}
}

// --- Contexts for Async Ops ---
struct ConnectContext : public AsyncContext {
	uv_connect_t req;
	ConnectContext(Fiber* f) : AsyncContext(f) { req.data = this; }
};
struct WriteContext : public AsyncContext {
	uv_write_t req;
	uv_buf_t buf;
	char* data;
	WriteContext(Fiber* f, const char* d, size_t len) : AsyncContext(f) {
		req.data = this;
		data = new char[len];
		memcpy(data, d, len);
		buf = uv_buf_init(data, len);
	}
	~WriteContext() { delete[] data; }
};
struct ReadContext : public AsyncContext {
	uv_buf_t buf; // We own this
	ReadContext(Fiber* f) : AsyncContext(f) {
		buf.base = new char[65536]; // Default 64k buffer
		buf.len = 65536;
	}
	~ReadContext() { delete[] buf.base; }
};
struct PollContext : public AsyncContext {
	uv_poll_t poll_handle;
	PollContext(Fiber* f) : AsyncContext(f) { poll_handle.data = this; }
};

// --- Callbacks ---
void OnUvConnection(uv_stream_t* server_handle, int status) {
	NetHandle* wrap = static_cast<NetHandle*>(server_handle->data);
	if (wrap->accept_queue.empty()) return;

	Fiber* fiber = wrap->accept_queue.front();
	wrap->accept_queue.pop();
	v8::Isolate* isolate = fiber->isolate();
	v8::HandleScope handle_scope(isolate);

	if (status < 0) {
		wrap->ResumeError(status, "accept");
	} else {
		auto client_handle = std::make_shared<NetHandle>();
		uv_tcp_init(Fiber::get_loop(), &client_handle->handle);
		if (uv_accept(server_handle, (uv_stream_t*)&client_handle->handle) == 0) {
			uint64_t id = HandleStore::Add(client_handle);
			wrap->Resume(v8::BigInt::New(isolate, id));
		} else {
			uv_close((uv_handle_t*)&client_handle->handle, [](uv_handle_t* h){ delete (uv_tcp_t*)h; });
			wrap->ResumeError(-1, "accept");
		}
	}
}
void OnConnect(uv_connect_t* req, int status) {
	ConnectContext* context = static_cast<ConnectContext*>(req->data);
	if (status < 0) context->ResumeError(status, "connect");
	else context->Resume(v8::Undefined(context->fiber->isolate()));
	delete context;
}
void OnWrite(uv_write_t* req, int status) {
	WriteContext* context = static_cast<WriteContext*>(req->data);
	if (status < 0) context->ResumeError(status, "write");
	else context->Resume(v8::BigInt::New(context->fiber->isolate(), context->buf.len));
	delete context;
}
void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(handle->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	*buf = context->buf; // Use the buffer we pre-allocated in ReadContext
}
void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(stream->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	if (context == nullptr) return; // Read came after close/error
	
	uv_read_stop(stream);
	wrap->pending_read = nullptr;
	
	if (nread < 0) {
		if (nread != UV_EOF) context->ResumeError(nread, "read");
		else context->Resume(v8::Null(context->fiber->isolate())); // EOF
	} else if (nread >= 0) {
		// We read data, resume with the count
		context->Resume(v8::BigInt::New(context->fiber->isolate(), nread));
	}
	// Do NOT delete context here, JS side needs to copy from context->buf.base
}
void OnPoll(uv_poll_t* handle, int status, int events) {
	PollContext* context = static_cast<PollContext*>(handle->data);
	uv_poll_stop(handle);
	uv_close((uv_handle_t*)handle, [](uv_handle_t* h){
		delete static_cast<PollContext*>(h->data);
	});
	
	if (status < 0) context->ResumeError(status, "poll");
	else context->Resume(v8::Integer::New(context->fiber->isolate(), events));
}

// --- JS Primitives ---
void TCP_Listen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value host_str(isolate, args[0]);
	int port = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = std::make_shared<NetHandle>();
	uv_tcp_init(Fiber::get_loop(), &wrap->handle);

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	int r = uv_tcp_bind(&wrap->handle, (const sockaddr*)&addr, 0);
	if (r) { ThrowUVException(isolate, r, "bind"); return; }
	
	r = uv_listen((uv_stream_t*)&wrap->handle, 128, OnUvConnection);
	if (r) { ThrowUVException(isolate, r, "listen"); return; }

	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}
void TCP_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid server handle"); return; }
	wrap->accept_queue.push(Fiber::get_current());
	Fiber::yield();
	args.GetReturnValue().Set(Fiber::get_current()->resume_value.Get(args.GetIsolate()));
}
void TCP_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value host_str(isolate, args[0]);
	int port = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = std::make_shared<NetHandle>();
	uv_tcp_init(Fiber::get_loop(), &wrap->handle);
	
	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);

	ConnectContext* context = new ConnectContext(Fiber::get_current());
	int r = uv_tcp_connect(&context->req, &wrap->handle, (const sockaddr*)&addr, OnConnect);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "connect", *host_str); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}
void TCP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }
	
	ReadContext* context = new ReadContext(Fiber::get_current());
	wrap->pending_read = context; // Store context

	uv_read_start((uv_stream_t*)&wrap->handle, OnAlloc, OnRead);
	Fiber::yield();
	
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) {
		delete context; // Delete context on error
		isolate->ThrowException(result);
		return;
	}
	if (result->IsNull()) {
		delete context; // Delete context on EOF
		args.GetReturnValue().Set(v8::Null(isolate)); // EOF
		return;
	}
	
	int64_t bytes_read = result.As<v8::BigInt>()->Int64Value();
	char* js_buf = GetUint8ArrayBufferData(buffer);
	size_t js_buf_len = GetUint8ArrayByteLength(buffer);
	
	size_t to_copy = std::min((size_t)bytes_read, js_buf_len);
	memcpy(js_buf, context->buf.base, to_copy);
	
	delete context; // Delete context after copying
	args.GetReturnValue().Set(v8::BigInt::New(isolate, (int64_t)to_copy));
}
void TCP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }

	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);

	WriteContext* context = new WriteContext(Fiber::get_current(), data, len);
	int r = uv_write(&context->req, (uv_stream_t*)&wrap->handle, &context->buf, 1, OnWrite);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write"); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}
void TCP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	HandleStore::Remove(id); // This finds the handle, calls Close(), and removes from map
}
void TCP_Poll(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	int events = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }
	
	uv_os_fd_t fd;
	if (uv_fileno(&wrap->handle, &fd) != 0) { ThrowUVException(isolate, UV_EBADF, "poll (fileno)"); return; }
	
	PollContext* context = new PollContext(Fiber::get_current());
	uv_poll_init_socket(Fiber::get_loop(), &context->poll_handle, fd);
	uv_poll_start(&context->poll_handle, events, OnPoll);
	
	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}
void InitializeTCP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tcp_obj = v8::Object::New(isolate); // Renamed
	SET_METHOD(tcp_obj, "listen", TCP_Listen);
	SET_METHOD(tcp_obj, "accept", TCP_Accept);
	SET_METHOD(tcp_obj, "connect", TCP_Connect);
	SET_METHOD(tcp_obj, "read", TCP_Read);
	SET_METHOD(tcp_obj, "write", TCP_Write);
	SET_METHOD(tcp_obj, "close", TCP_Close);
	SET_METHOD(tcp_obj, "poll", TCP_Poll);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tcp").ToLocalChecked(), tcp_obj).Check(); // Renamed
}

