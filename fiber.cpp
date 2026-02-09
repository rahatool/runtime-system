#include "fiber.h"
#include "primitives.h"
#include <iostream>

Fiber* Fiber::current_fiber_ = nullptr;
Fiber* Fiber::main_fiber_ = nullptr;
uv_loop_t* Fiber::event_loop_ = nullptr;

// --- Common Implementation ---

void Fiber::init(v8::Isolate* isolate, uv_loop_t* loop) {
	event_loop_ = loop;
	main_fiber_ = new Fiber(isolate);
}

void Fiber::set_main_context(v8::Local<v8::Context> context) {
	if (main_fiber_) {
		main_fiber_->v8_context_.Reset(main_fiber_->isolate_, context);
	}
}

Fiber* Fiber::get_current() { return current_fiber_; }
uv_loop_t* Fiber::get_loop() { return event_loop_; }

Fiber::~Fiber() {
	func_.Reset();
	resume_value.Reset();
	js_object.Reset();

#ifdef _WIN32
	if (context_ != main_fiber_->context_) {
		DeleteFiber(context_);
	}
#else
	delete[] stack_;
#endif
}

void Fiber::run() {
	v8::Isolate::Scope isolate_scope(isolate_);
	v8::HandleScope handle_scope(isolate_);
	v8::Local<v8::Context> context = isolate_->GetCurrentContext();
	v8_context_.Reset(isolate_, context);
	v8::Local<v8::Function> func = func_.Get(isolate_);
	v8::TryCatch try_catch(isolate_);

	v8::MaybeLocal<v8::Value> result = func->Call(context, context->Global(), 0, nullptr);

	if (result.IsEmpty() && try_catch.HasCaught()) {
		ReportException(isolate_, &try_catch);
	}
	state_ = DONE;
}

// --- Windows Implementation ---
#ifdef _WIN32

Fiber::Fiber(v8::Isolate* isolate) : isolate_(isolate), state_(NEW) {
	context_ = ConvertThreadToFiber(nullptr);
	current_fiber_ = this;
	state_ = RUNNING;
	js_object.Reset(isolate, v8::Object::New(isolate));
}

Fiber::Fiber(v8::Isolate* isolate, v8::Local<v8::Function> func) 
	: isolate_(isolate), state_(NEW) {
	func_.Reset(isolate, func);
	js_object.Reset(isolate, v8::Object::New(isolate));
	context_ = CreateFiber(STACK_SIZE, fiber_entry, this);
}

void Fiber::yield() {
	Fiber* yielding_fiber = current_fiber_;
	yielding_fiber->state_ = SUSPENDED;
	current_fiber_ = main_fiber_;
	SwitchToFiber(main_fiber_->context_);
}

void Fiber::resume(Fiber* fiber) {
	current_fiber_ = fiber;
	fiber->state_ = RUNNING;
	SwitchToFiber(fiber->context_);
	if (fiber->state_ == DONE) {
		delete fiber;
		current_fiber_ = main_fiber_;
	}
}

VOID CALLBACK Fiber::fiber_entry(PVOID lpParameter) {
	Fiber* self = static_cast<Fiber*>(lpParameter);
	self->run();
	yield();
}

#else // --- POSIX Implementation ---

Fiber::Fiber(v8::Isolate* isolate) : isolate_(isolate), state_(NEW), stack_(nullptr) {
	context_ = &uctx_;
	getcontext(context_);
	current_fiber_ = this;
	state_ = RUNNING;
	js_object.Reset(isolate, v8::Object::New(isolate));
}

Fiber::Fiber(v8::Isolate* isolate, v8::Local<v8::Function> func) 
	: isolate_(isolate), state_(NEW) {
	func_.Reset(isolate, func);
	js_object.Reset(isolate, v8::Object::New(isolate));
	context_ = &uctx_;
	getcontext(context_);
	stack_ = new char[STACK_SIZE];
	context_->uc_stack.ss_sp = stack_;
	context_->uc_stack.ss_size = STACK_SIZE;
	context_->uc_link = &main_fiber_->uctx_;
	makecontext(context_, fiber_entry, 0);
}

void Fiber::yield() {
	Fiber* yielding_fiber = current_fiber_;
	yielding_fiber->state_ = SUSPENDED;
	current_fiber_ = main_fiber_;
	swapcontext(yielding_fiber->context_, main_fiber_->context_);
}

void Fiber::resume(Fiber* fiber) {
	current_fiber_ = fiber;
	fiber->state_ = RUNNING;
	swapcontext(main_fiber_->context_, fiber->context_);
	if (fiber->state_ == DONE) {
		delete fiber;
		current_fiber_ = main_fiber_;
	}
}

void Fiber::fiber_entry() {
	current_fiber_->run();
}

#endif

// --- Primitives exposed to JS ---

struct SleepContext : public AsyncContext {
	uv_timer_t timer;
	SleepContext(Fiber* f) : AsyncContext(f) {
		timer.data = this;
	}
	// sleep() just resolves to undefined on success.
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		return v8::Undefined(isolate);
	}
};

void OnSleepTimer(uv_timer_t* handle) {
	SleepContext* context = static_cast<SleepContext*>(handle->data);
	context->result = 0; // Success
	
	// Don't resume directly. Queue for the main loop.
	QueueFiberToResume(context);

	uv_close((uv_handle_t*)handle, [](uv_handle_t* h){
		// The context is deleted in OnFibersToResume, not here.
		// We just need to free the handle's memory if it was allocated separately, which it isn't.
	});
}

void Fiber_Sleep(const v8::FunctionCallbackInfo<v8::Value>& args) {
	int64_t ms = args[0].As<v8::BigInt>()->Int64Value();

	SleepContext* context = new SleepContext(Fiber::get_current());
	uv_timer_init(Fiber::get_loop(), &context->timer);
	uv_timer_start(&context->timer, OnSleepTimer, ms, 0);

	Fiber::yield();
	// No return value needed, as sleep does not return anything.
}

void Fiber_Current(const v8::FunctionCallbackInfo<v8::Value>& args) {
	args.GetReturnValue().Set(Fiber::get_current()->js_object.Get(args.GetIsolate()));
}

void Fiber_Run(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Function> func = args[0].As<v8::Function>();
	Fiber* f = new Fiber(isolate, func);
	Fiber::resume(f);
}

void InitializeFibers(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fiber_obj = v8::Object::New(isolate);
	SET_METHOD(fiber_obj, "run", Fiber_Run);
	SET_METHOD(fiber_obj, "current", Fiber_Current);
	SET_METHOD(fiber_obj, "sleep", Fiber_Sleep);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "fiber").ToLocalChecked(), fiber_obj).Check();
}