#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

// --- Handle Implementation ---
void FileHandle::Close() {
	// Synchronous close for cleanup via HandleStore::Remove
	uv_fs_t req;
	uv_fs_close(Fiber::get_loop(), &req, fd, nullptr);
	uv_fs_req_cleanup(&req);
}
void DirHandle::Close() {
	// Synchronous close for cleanup via HandleStore::Remove
	uv_fs_t req;
	uv_fs_closedir(Fiber::get_loop(), &req, dir, nullptr);
	uv_fs_req_cleanup(&req);
}

// --- Async Context ---
struct FSContext : public AsyncContext {
	uv_fs_t req;
	uv_buf_t iov; // Only used for Write
	std::string path_str;
	std::vector<std::pair<std::string, uv_dirent_type_t>> dir_entries;
	uv_stat_t stat_buf;
	uv_dir_t* dir_ptr = nullptr; // Used for opendir

	FSContext(Fiber* f, const char* path = nullptr) : AsyncContext(f) {
		req.data = this;
		if (path) path_str = path;
		iov.base = nullptr;
	}
	~FSContext() {
		// For FS_Write, iov.base points into JS memory, don't delete.
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
			case UV_FS_OPENDIR:
				context->dir_ptr = (uv_dir_t*)req->ptr; // Store the dir handle
				context->Resume(v8::Undefined(isolate));
				break;
			case UV_FS_READDIR: {
				uv_dirent_t dent;
				int count = 0;
				while (uv_fs_readdir(req, &dent) != UV_EOF) {
					context->dir_entries.push_back({dent.name, dent.type});
					count++;
				}
				// If count is 0, we reached the end
				context->Resume(v8::Integer::New(isolate, count));
				break;
			}
			case UV_FS_STAT:
			case UV_FS_FSTAT:
				memcpy(&context->stat_buf, req->ptr, sizeof(uv_stat_t));
				context->Resume(v8::Undefined(isolate));
				break;
			default: // open, read, write, close, sync, unlink, mkdir, rmdir
				context->Resume(v8::BigInt::New(isolate, req->result));
				break;
		}
	}
	// We delete context here for most ops, but NOT for OPENDIR or READDIR
	if (req->fs_type != UV_FS_OPENDIR && req->fs_type != UV_FS_READDIR) {
		delete context;
	}
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
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	int64_t offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	// Point directly into the JS buffer view
	context->iov.base = GetUint8ArrayBufferData(buffer);
	context->iov.len = GetUint8ArrayByteLength(buffer);

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
	args.GetReturnValue().Set(result); // BigInt bytes read
}

void FS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	int64_t offset = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	// Point directly into the JS buffer view
	context->iov.base = GetUint8ArrayBufferData(buffer);
	context->iov.len = GetUint8ArrayByteLength(buffer);

	int r = uv_fs_write(Fiber::get_loop(), &context->req, handle->fd, &context->iov, 1, offset, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result); // BigInt bytes written
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

#define FS_SYNC_CALL(name, func) \
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

void FS_Sync(const v8::FunctionCallbackInfo<v8::Value>& args) { FS_SYNC_CALL("fsync", uv_fs_fsync); }
void FS_DataSync(const v8::FunctionCallbackInfo<v8::Value>& args) { FS_SYNC_CALL("fdatasync", uv_fs_fdatasync); }

#define FS_PATH_CALL(name, func, ...) \
	v8::Isolate* isolate = args.GetIsolate(); \
	v8::String::Utf8Value path(isolate, args[0]); \
	FSContext* context = new FSContext(Fiber::get_current(), *path); \
	int r = func(Fiber::get_loop(), &context->req, *path, ##__VA_ARGS__, OnFSCallback); \
	if (r < 0) { delete context; ThrowUVException(isolate, r, name, *path); return; } \
	Fiber::yield(); \
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate); \
	if(result->IsNativeError()) { isolate->ThrowException(result); return; } \
	args.GetReturnValue().Set(v8::Undefined(isolate));

void FS_Remove(const v8::FunctionCallbackInfo<v8::Value>& args) { FS_PATH_CALL("unlink", uv_fs_unlink); } // Renamed
void FS_DirMake(const v8::FunctionCallbackInfo<v8::Value>& args) {
	int mode = args[1]->Int32Value(args.GetIsolate()->GetCurrentContext()).ToChecked();
	FS_PATH_CALL("mkdir", uv_fs_mkdir, mode);
}
void FS_DirRemove(const v8::FunctionCallbackInfo<v8::Value>& args) { FS_PATH_CALL("rmdir", uv_fs_rmdir); }

void FS_DirOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value path(isolate, args[0]);

	FSContext* context = new FSContext(Fiber::get_current(), *path);
	int r = uv_fs_opendir(Fiber::get_loop(), &context->req, *path, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "opendir", *path); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { delete context; isolate->ThrowException(result); return; }

	auto handle = std::make_shared<DirHandle>(context->dir_ptr, *path);
	uint64_t id = HandleStore::Add(handle);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
	delete context; // Context is only needed to get dir_ptr back
}

void FS_DirRead(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirHandle>(id);
	if (!handle) { Throw(isolate, "Invalid directory handle"); return; }
	
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->req.dir.dir = handle->dir; // Use the stored dir handle

	int r = uv_fs_readdir(Fiber::get_loop(), &context->req, handle->dir, OnFSCallback);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "readdir", handle->path.c_str()); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { delete context; isolate->ThrowException(result); return; }

	int count = result.As<v8::Integer>()->Value();
	if (count == 0) {
		args.GetReturnValue().Set(v8::Null(isolate)); // End of directory
		delete context;
		return;
	}

	v8::Local<v8::Array> dir_array = v8::Array::New(isolate, context->dir_entries.size());
	for(size_t i = 0; i < context->dir_entries.size(); ++i) {
		v8::Local<v8::Object> entry = v8::Object::New(isolate);
		entry->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "name").ToLocalChecked(), v8::String::NewFromUtf8(isolate, context->dir_entries[i].first.c_str()).ToLocalChecked()).Check();
		entry->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "type").ToLocalChecked(), v8::Integer::New(isolate, context->dir_entries[i].second)).Check();
		dir_array->Set(isolate->GetCurrentContext(), i, entry).Check();
	}
	args.GetReturnValue().Set(dir_array);
	delete context;
}

void FS_DirClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	HandleStore::Remove(id); // Calls DirHandle::Close -> uv_fs_closedir
	args.GetReturnValue().Set(v8::Undefined(args.GetIsolate()));
}


void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fs_obj = v8::Object::New(isolate);
	SET_METHOD(fs_obj, "open", FS_Open);
	SET_METHOD(fs_obj, "read", FS_Read);
	SET_METHOD(fs_obj, "write", FS_Write);
	SET_METHOD(fs_obj, "close", FS_Close);
	SET_METHOD(fs_obj, "status", FS_Status); // Renamed
	SET_METHOD(fs_obj, "sync", FS_Sync); // Renamed
	SET_METHOD(fs_obj, "dataSync", FS_DataSync); // Renamed
	SET_METHOD(fs_obj, "remove", FS_Remove); // Renamed
	SET_METHOD(fs_obj, "dirOpen", FS_DirOpen);
	SET_METHOD(fs_obj, "dirRead", FS_DirRead);
	SET_METHOD(fs_obj, "dirClose", FS_DirClose);
	SET_METHOD(fs_obj, "dirMake", FS_DirMake);
	SET_METHOD(fs_obj, "dirRemove", FS_DirRemove);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "fs").ToLocalChecked(), fs_obj).Check();
}

