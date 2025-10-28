#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>

// --- Handle Implementation ---
void NetHandle::Close() {
	uv_close(&handle, [](uv_handle_t* h){
		// C++ object deleted via shared_ptr in HandleStore::Remove
	});
}

// --- Contexts for Async Ops ---
struct ConnectContext : public AsyncContext {
	uv_connect_t req;
	ConnectContext(Fiber* f) : AsyncContext(f) { req.data = this; }
};
struct WriteContext : public AsyncContext {
	uv_write_t req;
	uv_buf_t buf;
	v8::Persistent<v8::Uint8Array> user_buffer; // Keep buffer alive

	WriteContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf)
		: AsyncContext(f) {
		req.data = this;
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		// uv_write needs non-const, but won't modify
		buf = uv_buf_init(const_cast<char*>(data), len);
	}
	~WriteContext() {
		user_buffer.Reset();
	}
};
struct ReadContext : public AsyncContext {
	v8::Persistent<v8::Uint8Array> user_buffer; // Hold user buffer
	uv_buf_t iov; // Points into user_buffer

	ReadContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf)
		: AsyncContext(f) {
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		iov = uv_buf_init(data, len);
	}
	~ReadContext() {
		user_buffer.Reset();
	}
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
	// We read directly into the user buffer provided in TCP_Read
	NetHandle* wrap = static_cast<NetHandle*>(handle->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	*buf = context->iov;
}
void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(stream->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	if (context == nullptr) { return; } // Read stopped before callback

	uv_read_stop(stream);
	wrap->pending_read = nullptr;

	if (nread < 0) {
		if (nread != UV_EOF) context->ResumeError(nread, "read");
		else context->Resume(v8::BigInt::New(context->fiber->isolate(), -1)); // EOF is -1n
	} else if (nread >= 0) {
		// Data was already written into user buffer by OnAlloc/libuv
		context->Resume(v8::BigInt::New(context->fiber->isolate(), nread));
	}
	delete context;
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
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }
	if (wrap->pending_read) { Throw(isolate, "Read already pending on socket"); return; }

	ReadContext* context = new ReadContext(Fiber::get_current(), isolate, buffer_view);
	wrap->pending_read = context; // Store context

	uv_read_start((uv_stream_t*)&wrap->handle, OnAlloc, OnRead);
	Fiber::yield();

	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	args.GetReturnValue().Set(result); // Bytes read or -1n for EOF
}
void TCP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }

	WriteContext* context = new WriteContext(Fiber::get_current(), isolate, buffer_view);
	int r = uv_write(&context->req, (uv_stream_t*)&wrap->handle, &context->buf, 1, OnWrite);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write"); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result); // Bytes written
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
	if (uv_fileno(&wrap->handle, &fd) != 0) { ThrowUVException(isolate, -1, "poll (fileno)"); return; }

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

