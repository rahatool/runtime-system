#ifndef FIBER_H
#define FIBER_H

#ifdef _WIN32
#include <windows.h>
#else
#define _XOPEN_SOURCE
#include <ucontext.h>
#endif

#include "v8-persistent-handle.h"
#include "v8.h"
#include "uv.h"

#ifdef _WIN32
using platform_context_t = LPVOID;
#else
using platform_context_t = ucontext_t*;
#endif

class Fiber {
public:
	enum State { NEW, RUNNING, SUSPENDED, DONE };

	static void init(uv_loop_t* loop);
	static Fiber* get_current();
	static uv_loop_t* get_loop();

	Fiber(v8::Isolate* isolate, v8::Local<v8::Function> func);
	~Fiber();

	static void yield();
	static void resume(Fiber* fiber);
	State state() const { return state_; }
	v8::Isolate* isolate() const { return isolate_; }

	v8::Persistent<v8::Value> resume_value;
	v8::Persistent<v8::Object> js_object; // For Fiber.current()

private:
	// Private constructor for main fiber
	Fiber(); 
	void run();

	// Platform-specific entry points
#ifdef _WIN32
	static VOID CALLBACK fiber_entry(PVOID lpParameter);
#else
	static void fiber_entry();
#endif

	v8::Isolate* isolate_;
	v8::Persistent<v8::Function> func_;
	State state_;
	platform_context_t context_;

#ifndef _WIN32
	ucontext_t uctx_;
	char* stack_ = nullptr;
#endif

	static Fiber* current_fiber_;
	static Fiber* main_fiber_;
	static uv_loop_t* event_loop_;
	static const int STACK_SIZE = 1024 * 1024; // 1MB
};

#endif // FIBER_H

