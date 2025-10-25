#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#include <iostream>
#include <string>
#include <map>
#include <memory>
#include "v8.h"
#include "uv.h"
#include "fiber.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>

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

// Gets pointer and length from Uint8Array, performs bounds check
inline bool GetUint8ArrayData(v8::Local<v8::Uint8Array> arr, char** data, size_t* length) {
	 if (!arr->HasBuffer() || arr->Buffer().IsEmpty()) {
		 Throw(v8::Isolate::GetCurrent(), "Expected Uint8Array backed by ArrayBuffer");
		 return false;
	 }
	 v8::Local<v8::ArrayBuffer> ab = arr->Buffer();
	 if (ab.IsEmpty() || ab->ByteLength() < arr->ByteOffset() + arr->ByteLength()) {
		 Throw(v8::Isolate::GetCurrent(), "Invalid ArrayBuffer or view bounds");
		 return false;
	 }
	*data = static_cast<char*>(ab->GetContents().Data()) + arr->ByteOffset();
	*length = arr->ByteLength();
	return true;
}


// --- Async Contexts ---
struct AsyncContext {
	Fiber* fiber;
	AsyncContext(Fiber* f) : fiber(f) {}
	virtual ~AsyncContext() {} // Virtual destructor for safe cleanup

	void Resume(v8::Local<v8::Value> value) {
		fiber->resume_value.Reset(fiber->isolate(), value);
		Fiber::resume(fiber);
	}
	void ResumeError(int err, const char* syscall, const char* path = nullptr) {
		v8::Isolate* isolate = fiber->isolate();
		v8::HandleScope handle_scope(isolate);
		std::string msg = std::string(syscall) + " " + uv_strerror(err);
		if (path) { msg += " (" + std::string(path) + ")"; }
		v8::Local<v8::Value> error = v8::Exception::Error(v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked());
		fiber->resume_value.Reset(isolate, error);
		Fiber::resume(fiber);
	}
	void ResumeError(const char* message) {
		v8::Isolate* isolate = fiber->isolate();
		v8::HandleScope handle_scope(isolate);
		v8::Local<v8::Value> error = v8::Exception::Error(v8::String::NewFromUtf8(isolate, message).ToLocalChecked());
		fiber->resume_value.Reset(isolate, error);
		Fiber::resume(fiber);
	}
};

// --- Forward Declarations ---
void NET_Poll(const v8::FunctionCallbackInfo<v8::Value>& args); // From net, used by tls
void HANDLES_Free(const v8::FunctionCallbackInfo<v8::Value>& args); // Generic free
void InitializeHandles(v8::Isolate* isolate, v8::Local<v8::Object> exports);


#endif // PRIMITIVES_H

