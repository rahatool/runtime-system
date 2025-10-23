#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

// --- Handle Implementation ---
void FileHandle::Close() {
	// This is synchronous for cleanup.
	// The async FS_Close primitive will remove from map.
	uv_fs_t req;
	uv_fs_close(Fiber::get_loop(), &req, fd, nullptr);
	uv_fs_req_cleanup(&req);
}

// --- Async Context ---
struct FSContext : public AsyncContext {
	uv_fs_t req;
	uv_buf_t iov;
	std::string path_str;
	std::vector<std::string> dir_entries;
	uv_stat_t stat_buf;

	FSContext(Fiber* f, const char* path = nullptr) : AsyncContext(f) {
		req.data = this;
		if (path) path_str = path;
		iov.base = nullptr;
	}
	~FSContext() {
		delete[] iov.base;
		uv_fs_req_cleanup(&req);
	}
};

// --- Callbacks ---
void OnFSCallback(uv_fs_t* req) {
	FSContext* context = static_cast<FSContext*>(req->data);
	v8::Isolate* isolate = context->fiber->isolate();
	v8::HandleScope handle_scope(isolate);

	if (req->result < 0) {
		context->ResumeError(req->result, uv_fs_type_name(req->fs_type), context->path_str.c_str());
	} else {
		switch (req->fs_type) {
			case UV_FS_SCANDIR: {
				uv_dirent_t dent;
				while (uv_fs_scandir_next(req, &dent) != UV_EOF) {
					context->dir_entries.push_back(dent.name);
				}
				context->Resume(v8::Undefined(isolate));
				break;
			}
			case UV_FS_STAT:
			case UV_FS_FSTAT:
				memcpy(&context->stat_buf, req->ptr, sizeof(uv_stat_t));
				context->Resume(v8::Undefined(isolate));
				break;
			default:
				context->Resume(v8::BigInt::New(isolate, req->result));
				break;
		}
	}
	delete context;
}

// --- Primitives ---
void FS_Open(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value path(isolate, args[0]);
	int flags = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int mode = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	FSContext* context = new FSContext(Fiber::get_current(), *path);
	int r = uv_fs_open(Fiber::get_loop(), &context->req, *path, flags, mode, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "open", *path); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	uv_file fd = result.As<v8::BigInt>()->Int64Value();
	auto handle = std::make_shared<FileHandle>(fd, *path);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void FS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	int64_t len = args[1].As<v8::BigInt>()->Int64Value();
	int64_t offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->iov.base = new char[len];
	context->iov.len = len;

	int r = uv_fs_read(Fiber::get_loop(), &context->req, handle->fd, &context->iov, 1, offset, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "read", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	int64_t bytes_read = result.As<v8::BigInt>()->Int64Value();
	if (bytes_read == 0) {
		args.GetReturnValue().Set(v8::Null(isolate)); // EOF
		return;
	}

	v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, bytes_read);
	memcpy(ab->GetContents().Data(), context->iov.base, bytes_read);
	args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, bytes_read));
}

void FS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	int64_t offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	size_t len = buffer->ByteLength();
	char* data = (char*)buffer->Buffer()->GetContents().Data() + buffer->ByteOffset();
	context->iov = uv_buf_init(data, len);

	int r = uv_fs_write(Fiber::get_loop(), &context->req, handle->fd, &context->iov, 1, offset, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result); // Bytes written
}

void FS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_close(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "close", handle->path.c_str()); return; }

	Fiber::yield();
	HandleStore::Remove(id); // Remove from map after close completes
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(v8::Undefined(isolate));
}

v8::Local<v8::Object> FillStatObject(v8::Isolate* isolate, uv_stat_t* s) {
	v8::Local<v8::Object> obj = v8::Object::New(isolate);
	#define SET_STAT_FIELD(name, val) obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, #name).ToLocalChecked(), v8::BigInt::New(isolate, val)).Check()
	SET_STAT_FIELD(size, s->st_size);
	SET_STAT_FIELD(mode, s->st_mode);
	SET_STAT_FIELD(uid, s->st_uid);
	SET_STAT_FIELD(gid, s->st_gid);
	SET_STAT_FIELD(atimeMs, (s->st_atim.tv_sec * 1000) + (s->st_atim.tv_nsec / 1000000));
	SET_STAT_FIELD(mtimeMs, (s->st_mtim.tv_sec * 1000) + (s->st_mtim.tv_nsec / 1000000));
	return obj;
}

void FS_FStat(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fstat(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "fstat", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(FillStatObject(isolate, &context->stat_buf));
}

void FS_FSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fsync(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "fsync", handle->path.c_str()); return; }
	Fiber::yield();
}
void FS_FDataSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fdatasync(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "fdatasync", handle->path.c_str()); return; }
	Fiber::yield();
}

// ... Re-add readFile, readdir, unlink, mkdir, rmdir from previous implementation ...
// (They are functionally identical, just need to use new FSContext)

void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fs_obj = v8::Object::New(isolate);
	SET_METHOD(fs_obj, "open", FS_Open);
	SET_METHOD(fs_obj, "read", FS_Read);
	SET_METHOD(fs_obj, "write", FS_Write);
	SET_METHOD(fs_obj, "close", FS_Close);
	SET_METHOD(fs_obj, "fstat", FS_FStat);
	SET_METHOD(fs_obj, "fsync", FS_FSync);
	SET_METHOD(fs_obj, "fdatasync", FS_FDataSync);
	// ... re-add readFile, readdir, unlink, mkdir, rmdir
	exports->Set(context, v8::String::NewFromUtf8(isolate, "fs").ToLocalChecked(), fs_obj).Check();
}