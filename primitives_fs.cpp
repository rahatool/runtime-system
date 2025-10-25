#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

// --- Handle Implementations ---
void FileHandle::Close() {
	uv_fs_t req;
	uv_fs_close(Fiber::get_loop(), &req, fd, nullptr); // Synchronous close for cleanup
	uv_fs_req_cleanup(&req);
}
void DirectoryHandle::Close() {
	uv_fs_t req;
	uv_fs_closedir(Fiber::get_loop(), &req, dir, nullptr); // Synchronous close for cleanup
	uv_fs_req_cleanup(&req);
}

// --- Async Context ---
struct FSContext : public AsyncContext {
	uv_fs_t req;
	uv_buf_t iov; // Used for read/write directly into JS buffer view
	std::string path_str;
	std::vector<uv_dirent_t> dir_entries; // Store full dirent for DirectoryHandle.read
	uv_stat_t stat_buf;

	FSContext(Fiber* f, const char* path = nullptr) : AsyncContext(f) {
		req.data = this;
		if (path) path_str = path;
		iov.base = nullptr;
	}
	~FSContext() {
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
			case UV_FS_READDIR: { // Used by DirectoryHandle.read
				uv_dirent_t dent;
				while (uv_fs_readdir_next(req, &dent) == 0) {
					context->dir_entries.push_back(dent);
				}
				context->Resume(v8::Undefined(isolate));
				break;
			 }
			case UV_FS_OPENDIR: // Used by DirectoryHandle.open
				context->Resume(v8::Undefined(isolate)); // Success, dir handle is in req->ptr
				break;
			case UV_FS_STAT:
			case UV_FS_FSTAT: // Renamed C++ function uses UV_FS_FSTAT
				memcpy(&context->stat_buf, req->ptr, sizeof(uv_stat_t));
				context->Resume(v8::Undefined(isolate));
				break;
			// read, write, open, close, sync, etc. just return the result count/fd
			default:
				context->Resume(v8::BigInt::New(isolate, req->result));
				break;
		}
	}
	delete context;
}

// --- Primitives ---

// --- FileHandle Primitives ---
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

// Simplified Read: Operates on the full Uint8Array view passed
void FS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t file_offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	if (!GetUint8ArrayData(buffer_view, &context->iov.base, &context->iov.len)) return; // Error handled by GetUint8ArrayData

	int r = uv_fs_read(Fiber::get_loop(), &context->req, handle->fd, &context->iov, 1, file_offset, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "read", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	// Return bytes read or 0 for EOF
	if (result.As<v8::BigInt>()->Int64Value() == 0) {
		args.GetReturnValue().Set(v8::Null(isolate)); // Explicit EOF signal
	} else {
		args.GetReturnValue().Set(result); // Bytes read
	}
}

// Simplified Write: Operates on the full Uint8Array view passed
void FS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t file_offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	if (!GetUint8ArrayData(buffer_view, &context->iov.base, &context->iov.len)) return;

	// uv_fs_write needs non-const char*
	context->iov.base = const_cast<char*>(context->iov.base);

	int r = uv_fs_write(Fiber::get_loop(), &context->req, handle->fd, &context->iov, 1, file_offset, OnFSCallback);
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
	HandleStore::Remove(id); // Remove immediately from JS view
	if (r < 0) { delete context; ThrowUVException(isolate, r, "close", handle->path.c_str()); return; }

	Fiber::yield(); // Wait for C++ close to finish
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; } // Should not happen often for close
	args.GetReturnValue().Set(v8::Undefined(isolate));
}

v8::Local<v8::Object> FillStatObject(v8::Isolate* isolate, uv_stat_t* s) {
	v8::Local<v8::Object> obj = v8::Object::New(isolate);
	v8::Local<v8::Context> context = isolate->GetCurrentContext(); // Get current context
	#define SET_STAT_FIELD(name, val) obj->Set(context, v8::String::NewFromUtf8(isolate, #name).ToLocalChecked(), v8::BigInt::New(isolate, val)).Check()
	SET_STAT_FIELD(size, s->st_size);
	SET_STAT_FIELD(mode, s->st_mode);
	SET_STAT_FIELD(uid, s->st_uid);
	SET_STAT_FIELD(gid, s->st_gid);
	SET_STAT_FIELD(atimeMs, (uint64_t(s->st_atim.tv_sec) * 1000) + (s->st_atim.tv_nsec / 1000000));
	SET_STAT_FIELD(mtimeMs, (uint64_t(s->st_mtim.tv_sec) * 1000) + (s->st_mtim.tv_nsec / 1000000));
	SET_STAT_FIELD(ctimeMs, (uint64_t(s->st_ctim.tv_sec) * 1000) + (s->st_ctim.tv_nsec / 1000000));
	SET_STAT_FIELD(birthtimeMs, (uint64_t(s->st_birthtim.tv_sec) * 1000) + (s->st_birthtim.tv_nsec / 1000000));
	return obj;
}


// Renamed primitive
void FS_Status(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fstat(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "status", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(FillStatObject(isolate, &context->stat_buf));
}

// Renamed primitive
void FS_Sync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fsync(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "sync", handle->path.c_str()); return; }
	Fiber::yield();
}
// Renamed primitive
void FS_DataSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_fdatasync(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "dataSync", handle->path.c_str()); return; }
	Fiber::yield();
}

// --- Top-level FS Primitives (No Handle) ---
#define FS_ASYNC_NO_HANDLE_CALL(name, func, ...) \
	v8::Isolate* isolate = args.GetIsolate(); \
	v8::String::Utf8Value path(isolate, args[0]); \
	FSContext* context = new FSContext(Fiber::get_current(), *path); \
	int r = func(Fiber::get_loop(), &context->req, *path, ##__VA_ARGS__, OnFSCallback); \
	if (r < 0) { delete context; ThrowUVException(isolate, r, name, *path); return; } \
	Fiber::yield(); \
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate); \
	if(result->IsNativeError()) { isolate->ThrowException(result); return; } \
	args.GetReturnValue().Set(v8::Undefined(isolate));

void FS_Unlink(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_NO_HANDLE_CALL("unlink", uv_fs_unlink);
}

// --- DirectoryHandle Primitives ---
void FS_DirOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value path(isolate, args[0]);

	FSContext* context = new FSContext(Fiber::get_current(), *path);
	int r = uv_fs_opendir(Fiber::get_loop(), &context->req, *path, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "opendir", *path); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	uv_dir_t* dir = static_cast<uv_dir_t*>(context->req.ptr);
	auto handle = std::make_shared<DirectoryHandle>(dir, *path);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void FS_DirRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirectoryHandle>(id);
	if (!handle) { Throw(isolate, "Invalid directory handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_readdir(Fiber::get_loop(), &context->req, handle->dir, OnFSCallback);
	 if (r < 0) { delete context; ThrowUVException(isolate, r, "readdir", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	v8::Local<v8::Array> dir_array = v8::Array::New(isolate, context->dir_entries.size());
	for(size_t i = 0; i < context->dir_entries.size(); ++i) {
		v8::Local<v8::Object> entry = v8::Object::New(isolate);
		entry->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "name").ToLocalChecked(), v8::String::NewFromUtf8(isolate, context->dir_entries[i].name).ToLocalChecked()).Check();
		// Add type if needed (context->dir_entries[i].type)
		dir_array->Set(isolate->GetCurrentContext(), i, entry).Check();
	}
	// Return null if empty? Or empty array? Node returns empty array.
	if (context->dir_entries.empty()) {
		args.GetReturnValue().Set(v8::Null(isolate)); // Signal end
	} else {
		args.GetReturnValue().Set(dir_array);
	}
}

void FS_DirClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirectoryHandle>(id);
	if (!handle) { Throw(isolate, "Invalid directory handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	int r = uv_fs_closedir(Fiber::get_loop(), &context->req, handle->dir, OnFSCallback);
	HandleStore::Remove(id);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "closedir", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(v8::Undefined(isolate));
}

void FS_DirMake(const v8::FunctionCallbackInfo<v8::Value>& args) {
	int mode = args[1]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
	FS_ASYNC_NO_HANDLE_CALL("mkdir", uv_fs_mkdir, mode);
}
void FS_DirRemove(const v8::FunctionCallbackInfo<v8::Value>& args) {
	FS_ASYNC_NO_HANDLE_CALL("rmdir", uv_fs_rmdir);
}


void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fs_obj = v8::Object::New(isolate);
	// FileHandle methods (exposed under fs for now, JS class wraps)
	SET_METHOD(fs_obj, "open", FS_Open);
	SET_METHOD(fs_obj, "read", FS_Read);
	SET_METHOD(fs_obj, "write", FS_Write);
	SET_METHOD(fs_obj, "close", FS_Close);
	SET_METHOD(fs_obj, "status", FS_Status);     // Renamed
	SET_METHOD(fs_obj, "sync", FS_Sync);       // Renamed
	SET_METHOD(fs_obj, "dataSync", FS_DataSync); // Renamed
	// DirectoryHandle methods
	SET_METHOD(fs_obj, "dirOpen", FS_DirOpen);
	SET_METHOD(fs_obj, "dirRead", FS_DirRead);
	SET_METHOD(fs_obj, "dirClose", FS_DirClose);
	SET_METHOD(fs_obj, "dirMake", FS_DirMake);
	SET_METHOD(fs_obj, "dirRemove", FS_DirRemove);
	// Standalone
	SET_METHOD(fs_obj, "unlink", FS_Unlink);

	exports->Set(context, v8::String::NewFromUtf8(isolate, "fs").ToLocalChecked(), fs_obj).Check();
}

