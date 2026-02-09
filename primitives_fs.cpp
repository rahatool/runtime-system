#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

// FIX: Helper function to convert uv_fs_type enum to a string for error reporting.
const char* FSSyscallName(uv_fs_type type) {
    switch (type) {
        case UV_FS_OPEN: return "open";
        case UV_FS_CLOSE: return "close";
        case UV_FS_READ: return "read";
        case UV_FS_WRITE: return "write";
        case UV_FS_UNLINK: return "unlink";
        case UV_FS_MKDIR: return "mkdir";
        case UV_FS_RMDIR: return "rmdir";
        case UV_FS_SCANDIR: return "scandir";
        case UV_FS_STAT: return "stat";
        case UV_FS_FSTAT: return "fstat";
        case UV_FS_FSYNC: return "fsync";
        case UV_FS_FDATASYNC: return "fdatasync";
        default: return "fs_op";
    }
}

// --- Handle Implementation ---
void FileHandle::Close() {
	uv_fs_t req;
	uv_fs_close(Fiber::get_loop(), &req, fd, nullptr);
	uv_fs_req_cleanup(&req);
}

void DirectoryHandle::Close() {
    for (auto& entry : entries) {
        if (entry.name != nullptr) {
            free((void*)entry.name);
            entry.name = nullptr;
        }
    }
	entries.clear();
}

// --- Async Context ---
struct FSContext : public AsyncContext {
	uv_fs_t req;
	std::string path_str;
	std::vector<uv_dirent_t> dir_entries;
	uv_stat_t stat_buf;
	v8::Persistent<v8::Uint8Array> user_buffer;

	FSContext(Fiber* f, const char* path = nullptr) : AsyncContext(f) {
		req.data = this;
		if (path) path_str = path;
	}
	~FSContext() {
		user_buffer.Reset();
		uv_fs_req_cleanup(&req);
	}

	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
            // FIX: Use the new helper function instead of the non-existent uv_fs_type_name.
			error_syscall = FSSyscallName(req.fs_type);
			error_path = path_str;
			return AsyncContext::CreateResultValue(isolate);
		}

		switch (req.fs_type) {
			case UV_FS_OPEN: {
				auto handle = std::make_shared<FileHandle>(result, path_str.c_str());
				uint64_t id = HandleStore::Add(handle);
				return v8::BigInt::New(isolate, id);
			}
			case UV_FS_READ:
				return v8::BigInt::New(isolate, result == 0 ? -1 : result);
			case UV_FS_WRITE:
				return v8::BigInt::New(isolate, result);
			case UV_FS_SCANDIR: {
				auto handle = std::make_shared<DirectoryHandle>(std::move(dir_entries));
				uint64_t id = HandleStore::Add(handle);
				return v8::BigInt::New(isolate, id);
			}
			case UV_FS_STAT:
			case UV_FS_FSTAT: {
				memcpy(&stat_buf, req.ptr, sizeof(uv_stat_t));
				v8::Local<v8::Object> obj = v8::Object::New(isolate);
				#define SET_STAT_FIELD(name, val) obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, #name).ToLocalChecked(), v8::BigInt::New(isolate, val)).Check()
				SET_STAT_FIELD(size, stat_buf.st_size);
				SET_STAT_FIELD(mode, stat_buf.st_mode);
				SET_STAT_FIELD(uid, stat_buf.st_uid);
				SET_STAT_FIELD(gid, stat_buf.st_gid);
				SET_STAT_FIELD(atimeMs, (stat_buf.st_atim.tv_sec * 1000) + (stat_buf.st_atim.tv_nsec / 1000000));
				SET_STAT_FIELD(mtimeMs, (stat_buf.st_mtim.tv_sec * 1000) + (stat_buf.st_mtim.tv_nsec / 1000000));
				return obj;
			}
			default:
				return v8::Undefined(isolate);
		}
	}
};

// --- Callback ---
void OnFSCallback(uv_fs_t* req) {
	FSContext* context = static_cast<FSContext*>(req->data);
	context->result = req->result;

	if (req->fs_type == UV_FS_SCANDIR && req->result >= 0) {
		uv_dirent_t dent;
		while (uv_fs_scandir_next(req, &dent) != UV_EOF) {
			uv_dirent_t entry;
			entry.name = strdup(dent.name);
			entry.type = dent.type;
			context->dir_entries.push_back(entry);
		}
	}

	QueueFiberToResume(context);
}

// --- Primitives ---
void FS_Open(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value path(isolate, args[0]);
	int flags = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int mode = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	FSContext* context = new FSContext(Fiber::get_current(), *path);
	uv_fs_open(Fiber::get_loop(), &context->req, *path, flags, mode, OnFSCallback);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void FS_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t position = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->user_buffer.Reset(isolate, buffer_view);

	size_t len;
	char* data = GetUint8ArrayBufferData(buffer_view, &len);
	uv_buf_t iov = uv_buf_init(data, len);

	uv_fs_read(Fiber::get_loop(), &context->req, handle->fd, &iov, 1, position, OnFSCallback);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void FS_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	int64_t position = args[2].As<v8::BigInt>()->Int64Value();

	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	context->user_buffer.Reset(isolate, buffer_view);

	size_t len;
	char* data = GetUint8ArrayBufferData(buffer_view, &len);
	uv_buf_t iov = uv_buf_init(const_cast<char*>(data), len);

	uv_fs_write(Fiber::get_loop(), &context->req, handle->fd, &iov, 1, position, OnFSCallback);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void FS_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	uv_fs_close(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);

	Fiber::yield();
	HandleStore::Remove(id);
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(v8::Undefined(isolate));
}

void FS_Status(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<FileHandle>(id);
	if (!handle) { Throw(isolate, "Invalid file handle"); return; }

	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str());
	uv_fs_fstat(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

#define FS_ASYNC_HANDLE_ONLY_CALL(name_str, func) \
	v8::Isolate* isolate = args.GetIsolate(); \
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value(); \
	auto handle = HandleStore::Get<FileHandle>(id); \
	if (!handle) { Throw(isolate, "Invalid file handle"); return; } \
	FSContext* context = new FSContext(Fiber::get_current(), handle->path.c_str()); \
	func(Fiber::get_loop(), &context->req, handle->fd, OnFSCallback); \
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

#define FS_ASYNC_PATH_ONLY_CALL(name_str, func, ...) \
	v8::Isolate* isolate = args.GetIsolate(); \
	v8::String::Utf8Value path(isolate, args[0]); \
	FSContext* context = new FSContext(Fiber::get_current(), *path); \
	func(Fiber::get_loop(), &context->req, *path, ##__VA_ARGS__, OnFSCallback); \
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

	uv_fs_scandir(Fiber::get_loop(), &context->req, *path, 0, OnFSCallback);
	
	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
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
    free((void*)dent.name);
	dent.name = nullptr;
	args.GetReturnValue().Set(entry);
}

void FS_DirClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto handle = HandleStore::Get<DirectoryHandle>(id);
	if (!handle) { Throw(args.GetIsolate(), "Invalid directory handle"); return; }
	
	handle->Close();
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