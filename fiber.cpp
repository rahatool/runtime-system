#include "fiber.h"
#include "primitives.h"
#include <iostream>

Fiber* Fiber::current_fiber_ = nullptr;
Fiber* Fiber::main_fiber_ = nullptr;
uv_loop_t* Fiber::event_loop_ = nullptr;

// --- Common Implementation ---

void Fiber::init(uv_loop_t* loop) {
	event_loop_ = loop;
	main_fiber_ = new Fiber(); // Create the main fiber
}

Fiber* Fiber::get_current() { return current_fiber_; }
uv_loop_t* Fiber::get_loop() { return event_loop_; }

Fiber::~Fiber() {
	func_.Reset();
	resume_value.Reset();

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
	v8::Context::Scope context_scope(context);
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

Fiber::Fiber() : isolate_(nullptr), state_(NEW) {
	// Convert the main thread to a fiber
	context_ = ConvertThreadToFiber(nullptr);
	current_fiber_ = this;
	state_ = RUNNING;
}

Fiber::Fiber(v8::Isolate* isolate, v8::Local<v8::Function> func) 
	: isolate_(isolate), state_(NEW) {
	func_.Reset(isolate, func);
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

Fiber::Fiber() : isolate_(nullptr), state_(NEW), stack_(nullptr) {
	context_ = &uctx_;
	getcontext(context_);
	current_fiber_ = this;
	state_ = RUNNING;
}

Fiber::Fiber(v8::Isolate* isolate, v8::Local<v8::Function> func) 
	: isolate_(isolate), state_(NEW) {
	func_.Reset(isolate, func);
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

void InitializeFibers(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> fiber_obj = v8::Object::New(isolate);
	SET_METHOD(fiber_obj, "run", [](const v8::FunctionCallbackInfo<v8::Value>& args){
		Fiber* f = new Fiber(args.GetIsolate(), args[0].As<v8::Function>());
		Fiber::resume(f);
	});
	exports->Set(context, v8::String::NewFromUtf8(isolate, "fiber").ToLocalChecked(), fiber_obj).Check();
}