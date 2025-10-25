#include "primitives.h"

void ENCODING_Decode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> buffer = args[0].As<v8::Uint8Array>();
	char* data = GetUint8ArrayBufferData(buffer);
	size_t length = GetUint8ArrayBufferLength(buffer);
	
	v8::MaybeLocal<v8::String> str = v8::String::NewFromUtf8(isolate, data, v8::NewStringType::kNormal, length);
	args.GetReturnValue().Set(str.ToLocalChecked());
}

void ENCODING_Encode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value str(isolate, args[0]);
	size_t length = str.length();

	v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, length);
	memcpy(ab->GetContents().Data(), *str, length);
	
	args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, length));
}

void InitializeEncoding(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> encoding_obj = v8::Object::New(isolate);
	SET_METHOD(encoding_obj, "decode", ENCODING_Decode);
	SET_METHOD(encoding_obj, "encode", ENCODING_Encode);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "encoding").ToLocalChecked(), encoding_obj).Check();
}
