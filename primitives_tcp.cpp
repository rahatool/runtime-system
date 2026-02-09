#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>

// --- Handle Implementation ---
void NetHandle::Close() {
	if (!uv_is_closing((uv_handle_t*)&handle)) {
		uv_close((uv_handle_t*)&handle, [](uv_handle_t* h){});
	}
}

// --- Contexts for Async Ops ---
struct ConnectContext : public AsyncContext {
	uv_connect_t req;
	ConnectContext(Fiber* f) : AsyncContext(f) { req.data = this; }
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "connect";
			return AsyncContext::CreateResultValue(isolate);
		}
		auto client_handle = std::make_shared<NetHandle>();
		// The handle is already connected, just need to init our wrapper
		uv_tcp_init(Fiber::get_loop(), &client_handle->handle);
		// This is a bit of a hack: req.handle is the connected stream.
		client_handle->handle = *(reinterpret_cast<uv_tcp_t*>(req.handle));
		uint64_t id = HandleStore::Add(client_handle);
		return v8::BigInt::New(isolate, id);
	}
};

struct WriteContext : public AsyncContext {
	uv_write_t req;
	uv_buf_t buf;
	v8::Persistent<v8::Uint8Array> user_buffer;

	WriteContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf) : AsyncContext(f) {
		req.data = this;
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		buf = uv_buf_init(const_cast<char*>(data), len);
	}
	~WriteContext() { user_buffer.Reset(); }

	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "write";
			return AsyncContext::CreateResultValue(isolate);
		}
		// On success, return bytes written
		return v8::BigInt::New(isolate, buf.len);
	}
};

struct ReadContext : public AsyncContext {
	v8::Persistent<v8::Uint8Array> user_buffer;
	uv_buf_t iov;

	ReadContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf) : AsyncContext(f) {
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		iov = uv_buf_init(data, len);
	}
	~ReadContext() { user_buffer.Reset(); }
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			if (result == UV_EOF) return v8::BigInt::New(isolate, -1);
			error_syscall = "read";
			return AsyncContext::CreateResultValue(isolate);
		}
		return v8::BigInt::New(isolate, result);
	}
};

struct AcceptContext : public AsyncContext {
	uv_stream_t* server_handle;
	AcceptContext(Fiber* f, uv_stream_t* server) : AsyncContext(f), server_handle(server) {}

	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "accept";
			return AsyncContext::CreateResultValue(isolate);
		}
		auto client_handle = std::make_shared<NetHandle>();
		uv_tcp_init(Fiber::get_loop(), &client_handle->handle);
		if (uv_accept(server_handle, (uv_stream_t*)&client_handle->handle) == 0) {
			uint64_t id = HandleStore::Add(client_handle);
			return v8::BigInt::New(isolate, id);
		} else {
			uv_close((uv_handle_t*)&client_handle->handle, nullptr);
			result = -1; // Indicate error
			error_syscall = "accept (internal)";
			return AsyncContext::CreateResultValue(isolate);
		}
	}
};

struct PollContext : public AsyncContext {
	uv_poll_t poll_handle;
	PollContext(Fiber* f) : AsyncContext(f) { poll_handle.data = this; }
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "poll";
			return AsyncContext::CreateResultValue(isolate);
		}
		return v8::Integer::New(isolate, result);
	}
};

// --- Callbacks ---
void OnUvConnection(uv_stream_t* server_handle, int status) {
	NetHandle* wrap = static_cast<NetHandle*>(server_handle->data);
	if (wrap->accept_queue.empty()) return;

	Fiber* fiber = wrap->accept_queue.front();
	wrap->accept_queue.pop();
	
	AcceptContext* context = new AcceptContext(fiber, server_handle);
	context->result = status;
	QueueFiberToResume(context);
}

void OnConnect(uv_connect_t* req, int status) {
	ConnectContext* context = static_cast<ConnectContext*>(req->data);
	context->result = status;
	QueueFiberToResume(context);
}

void OnWrite(uv_write_t* req, int status) {
	WriteContext* context = static_cast<WriteContext*>(req->data);
	context->result = status;
	QueueFiberToResume(context);
}

void OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(handle->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	*buf = context->iov;
}

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(stream->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	if (context == nullptr) return;

	uv_read_stop(stream);
	wrap->pending_read = nullptr;
	
	context->result = nread;
	QueueFiberToResume(context);
}

void OnPoll(uv_poll_t* handle, int status, int events) {
	PollContext* context = static_cast<PollContext*>(handle->data);
	uv_poll_stop(handle);
	uv_close((uv_handle_t*)handle, [](uv_handle_t* h){
		// The context is deleted by the main loop.
	});

	context->result = (status < 0) ? status : events;
	QueueFiberToResume(context);
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
	if (uv_tcp_bind(&wrap->handle, (const sockaddr*)&addr, 0) != 0) { Throw(isolate, "Failed to bind"); return; }
	if (uv_listen((uv_stream_t*)&wrap->handle, 128, OnUvConnection) != 0) { Throw(isolate, "Failed to listen"); return; }

	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void TCP_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid server handle"); return; }

	wrap->accept_queue.push(Fiber::get_current());
	
	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(args.GetIsolate());
	if (result->IsNativeError()) { args.GetIsolate()->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void TCP_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value host_str(isolate, args[0]);
	int port = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	uv_tcp_t* socket = new uv_tcp_t(); // Will be managed by the ConnectContext
	uv_tcp_init(Fiber::get_loop(), socket);

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);

	ConnectContext* context = new ConnectContext(Fiber::get_current());
	uv_tcp_connect(&context->req, socket, (const sockaddr*)&addr, OnConnect);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	delete socket;
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void TCP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }
	if (wrap->pending_read) { Throw(isolate, "Read already pending on socket"); return; }

	ReadContext* context = new ReadContext(Fiber::get_current(), isolate, buffer_view);
	wrap->pending_read = context;

	uv_read_start((uv_stream_t*)&wrap->handle, OnAlloc, OnRead);
	Fiber::yield();

	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void TCP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }

	WriteContext* context = new WriteContext(Fiber::get_current(), isolate, buffer_view);
	uv_write(&context->req, (uv_stream_t*)&wrap->handle, &context->buf, 1, OnWrite);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void TCP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid socket handle"); return; }
	wrap->Close();
	HandleStore::Remove(id);
}

void TCP_Poll(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	int events = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }

    uv_os_fd_t fd;
    if (uv_fileno((const uv_handle_t*)&wrap->handle, &fd) != 0) { Throw(isolate, "Failed to get fileno for poll"); return; }

	PollContext* context = new PollContext(Fiber::get_current());
    uv_poll_init_socket(Fiber::get_loop(), &context->poll_handle, (uv_os_sock_t) (uintptr_t) fd);
	uv_poll_start(&context->poll_handle, events, OnPoll);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void InitializeTCP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tcp_obj = v8::Object::New(isolate);
	SET_METHOD(tcp_obj, "listen", TCP_Listen);
	SET_METHOD(tcp_obj, "accept", TCP_Accept);
	SET_METHOD(tcp_obj, "connect", TCP_Connect);
	SET_METHOD(tcp_obj, "read", TCP_Read);
	SET_METHOD(tcp_obj, "write", TCP_Write);
	SET_METHOD(tcp_obj, "close", TCP_Close);
	SET_METHOD(tcp_obj, "poll", TCP_Poll);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tcp").ToLocalChecked(), tcp_obj).Check();
}