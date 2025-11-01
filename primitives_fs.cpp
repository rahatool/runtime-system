#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

// --- Handle Implementation ---
void FileHandle::Close() {
	// This is synchronous for cleanup on exit.
	// The async FS_Close primitive will remove from map.
	uv_fs_t req;
	uv_fs_close(Fiber::get_loop(), &req, fd, nullptr);
	uv_fs_req_cleanup(&req);
}

void DirectoryHandle::Close() {
	// Free remaining string copies
    for (auto& entry : entries) {
        free((void*)entry.name);
    }
	entries.clear();
}

// --- Async Context ---
struct FSContext : public AsyncContext {
	uv_fs_t req;
	// uv_buf_t iov; // No longer needed, we use user's buffer
	std::string path_str;
	std::vector<uv_dirent_t> dir_entries;
	uv_stat_t stat_buf;
	v8::Persistent<v8::Uint8Array> user_buffer; // Hold user buffer for read/write

	FSContext(Fiber* f, const char* path = nullptr) : AsyncContext(f) {
		req.data = this;
		if (path) path_str = path;
		// iov.base = nullptr;
	}
	~FSContext() {
		user_buffer.Reset();
		uv_fs_req_cleanup(&req);
	}
};

// --- Callbacks ---
void OnFSCallback(uv_fs_t* req) {
	FSContext* context = static_cast<FSContext*>(req->data);
	v8::Isolate* isolate = context->fiber->isolate();
	v8::HandleScope handle_scope(isolate);

    if (req->result < 0) {
        context->ResumeError(req->result, "fs", context->path_str.c_str());
	} else {
		switch (req->fs_type) {
			case UV_FS_READ:
				// Return bytes read (-1n for EOF)
				context->Resume(v8::BigInt::New(isolate, req->result == 0 ? -1 : req->result));
				break;
			case UV_FS_SCANDIR: {
				uv_dirent_t dent;
				while (uv_fs_scandir_next(req, &dent) != UV_EOF) {
					// Make a copy of the dirent
					uv_dirent_t entry;
					entry.name = strdup(dent.name);
					entry.type = dent.type;
					context->dir_entries.push_back(entry);
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
				// For open, write, close, sync
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
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t position = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->user_buffer.Reset(isolate, buffer_view); // Keep buffer alive

	size_t len;
	char* data = GetUint8ArrayBufferData(buffer_view, &len);
	uv_buf_t iov = uv_buf_init(data, len); // Use user buffer directly

	int r = uv_fs_read(Fiber::get_loop(), &context->req, handle->fd, &iov, 1, position, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "read", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	args.GetReturnValue().Set(result); // Bytes read or -1n for EOF
}

void FS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t position = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->user_buffer.Reset(isolate, buffer_view); // Keep buffer alive

	size_t len;
	// V8 buffer data is const, but uv_fs_write won't modify it.
	char* data = GetUint8ArrayBufferData(buffer_view, &len);
	uv_buf_t iov = uv_buf_init(const_cast<char*>(data), len); // Use user buffer directly

	int r = uv_fs_write(Fiber::get_loop(), &context->req, handle->fd, &iov, 1, position, OnFSCallback);
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

void FS_Status(const v8::FunctionCallbackInfo<v8::Value>& args) {
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

#define FS_ASYNC_HANDLE_ONLY_CALL(name, func) \
	v8::Isolate* isolate = args.GetIsolate(); \
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value(); \
	auto handle = HandleStore::Get<FileHandle>(id); \
	if (!handle) { Throw(isolate, "Invalid file handle"); return; } \
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str()); \
	int r = func(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback); \
	if (r < 0) { delete context; ThrowUVException(isolate, r, name, handle->path.c_str()); return; } \
	Fiber::yield(); \
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate); \
	if(result->IsNativeError()) { isolate->ThrowException(result); return; } \
	args.GetReturnValue().Set(v8::Undefined(isolate));

void FS_Sync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_HANDLE_ONLY_CALL("fsync", uv_fs_fsync);
}
void FS_DataSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_HANDLE_ONLY_CALL("fdatasync", uv_fs_fdatasync);
}

#define FS_ASYNC_PATH_ONLY_CALL(name, func, ...) \
	v8::Isolate* isolate = args.GetIsolate(); \
	v8::String::Utf8Value path(isolate, args[0]); \
	FSContext* context = new FSContext(Fiber::get_current(), *path); \
	int r = func(Fiber::get_loop(), &context->req, *path, ##__VA_ARGS__, OnFSCallback); \
	if (r < 0) { delete context; ThrowUVException(isolate, r, name, *path); return; } \
	Fiber::yield(); \
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate); \
	if(result->IsNativeError()) { isolate->ThrowException(result); return; } \
	args.GetReturnValue().Set(v8::Undefined(isolate));

void FS_Remove(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_PATH_ONLY_CALL("unlink", uv_fs_unlink);
}
void FS_DirMake(const v8::FunctionCallbackInfo<v8::Value>& args) {
	int mode = args[1]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
	FS_ASYNC_PATH_ONLY_CALL("mkdir", uv_fs_mkdir, mode);
}
void FS_DirRemove(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_PATH_ONLY_CALL("rmdir", uv_fs_rmdir);
}

void FS_DirOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value path(isolate, args[0]);
	FSContext* context = new FSContext(Fiber::get_current(), *path);

	int r = uv_fs_scandir(Fiber::get_loop(), &context->req, *path, 0, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "scandir", *path); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	// Steal the vector of entries from the context before it's deleted
	auto handle = std::make_shared<DirectoryHandle>(std::move(context->dir_entries));
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void FS_DirRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirectoryHandle>(id);
	if (!handle) { Throw(isolate, "Invalid directory handle"); return; }

	if (handle->index >= handle->entries.size()) {
		args.GetReturnValue().Set(v8::Null(isolate));
		return;
	}

	uv_dirent_t& dent = handle->entries[handle->index++];
	v8::Local<v8::Object> entry = v8::Object::New(isolate);
	entry->Set(context, v8::String::NewFromUtf8(isolate, "name").ToLocalChecked(), v8::String::NewFromUtf8(isolate, dent.name).ToLocalChecked()).Check();
	entry->Set(context, v8::String::NewFromUtf8(isolate, "type").ToLocalChecked(), v8::Integer::New(isolate, dent.type)).Check();
    free((void*)dent.name); // Free the strdup'd name
	args.GetReturnValue().Set(entry);
}

void FS_DirClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirectoryHandle>(id);
	if (!handle) { Throw(args.GetIsolate(), "Invalid directory handle"); return; }
	
	handle->Close(); // Frees remaining strings
	HandleStore::Remove(id);
}

void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fs_obj = v8::Object::New(isolate);
	SET_METHOD(fs_obj, "open", FS_Open);
	SET_METHOD(fs_obj, "read", FS_Read);
	SET_METHOD(fs_obj, "write", FS_Write);
	SET_METHOD(fs_obj, "close", FS_Close);
	SET_METHOD(fs_obj, "status", FS_Status);
	SET_METHOD(fs_obj, "sync", FS_Sync);
	SET_METHOD(fs_obj, "dataSync", FS_DataSync);
	SET_METHOD(fs_obj, "remove", FS_Remove);
	SET_METHOD(fs_obj, "dirOpen", FS_DirOpen);
	SET_METHOD(fs_obj, "dirRead", FS_DirRead);
	SET_METHOD(fs_obj, "dirClose", FS_DirClose);
	SET_METHOD(fs_obj, "dirMake", FS_DirMake);
	SET_METHOD(fs_obj, "dirRemove", FS_DirRemove);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "fs").ToLocalChecked(), fs_obj).Check();
}

