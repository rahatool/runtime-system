#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#include <iostream>
#include <string>
#include <map>
#include <memory>
#include "v8.h"
#include "uv.h"
#include "fiber.h"

// --- Helper to throw V8 exceptions ---
inline void Throw(v8::Isolate* isolate, const char* message) {
	isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(isolate, message).ToLocalChecked()));
}
inline void ThrowUVException(v8::Isolate* isolate, int uv_errno, const char* syscall, const char* path = nullptr) {
	std::string msg = std::string(syscall) + " " + uv_strerror(uv_errno);
	if (path) {
		msg += " (" + std::string(path) + ")";
	}
	isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked()));
}

// --- Exception Reporting ---
inline void ReportException(v8::Isolate* isolate, v8::TryCatch* try_catch) {
	v8::HandleScope handle_scope(isolate);
	v8::String::Utf8Value exception(isolate, try_catch->Exception());
	v8::Local<v8::Message> message = try_catch->Message();
	if (message.IsEmpty()) {
		std::cerr << *exception << std::endl;
	} else {
		v8::Local<v8::Context> context(isolate->GetCurrentContext());
		v8::String::Utf8Value filename(isolate, message->GetScriptOrigin().ResourceName());
		int linenum = message->GetLineNumber(context).FromMaybe(-1);
		std::cerr << *filename << ":" << linenum << ": " << *exception << std::endl;
		v8::MaybeLocal<v8::Value> stack_trace_maybe = try_catch->StackTrace(context);
		if (!stack_trace_maybe.IsEmpty()) {
			v8::String::Utf8Value stack_trace(isolate, stack_trace_maybe.ToLocalChecked());
			if (stack_trace.length() > 0) {
				std::cerr << *stack_trace << std::endl;
			}
		}
	}
}

// --- V8 Helpers ---
#define SET_METHOD(obj, name, func) obj->Set(context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(), v8::FunctionTemplate::New(isolate, func)->GetFunction(context).ToLocalChecked()).Check()

inline char* GetUint8ArrayBufferData(v8::Local<v8::Uint8Array> arr, size_t* len) {
    *len = arr->ByteLength();
    std::shared_ptr<v8::BackingStore> backing = arr->Buffer()->GetBackingStore();
    return static_cast<char*>(backing->Data()) + arr->ByteOffset();
}

inline char* GetUint8ArrayBufferData(v8::Local<v8::Value> val, size_t* len) {
    v8::Local<v8::Uint8Array> arr = val.As<v8::Uint8Array>();
    *len = arr->ByteLength();
    std::shared_ptr<v8::BackingStore> backing = arr->Buffer()->GetBackingStore();
    return static_cast<char*>(backing->Data()) + arr->ByteOffset();
}

// --- Async Contexts ---
struct AsyncContext {
	Fiber* fiber;
	ssize_t result; // Store the raw integer result from libuv
	std::string error_path;
	std::string error_syscall;

	AsyncContext(Fiber* f) : fiber(f), result(0) {}
	virtual ~AsyncContext() {} // Virtual destructor for safe cleanup

	// This virtual method is called by the main loop (OnFibersToResume) 
	// to convert the stored result into a V8 value.
	virtual v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) {
		if (result < 0) {
			// Create a V8 Error object if the result was a libuv error code.
			std::string msg = error_syscall + " " + uv_strerror(result);
			if (!error_path.empty()) {
				msg += " (" + error_path + ")";
			}
			return v8::Exception::Error(v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked());
		}
		// The default success value is just the integer result.
		return v8::BigInt::New(isolate, result);
	}
};

// Forward declarations
void TCP_Poll(const v8::FunctionCallbackInfo<v8::Value>& args);
void QueueFiberToResume(AsyncContext* context); // Make the helper available to all primitive files

#endif // PRIMITIVES_H