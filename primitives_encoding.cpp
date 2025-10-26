#include "primitives.h"

// Uses V8's built-in, highly optimized UTF-8 encoder
void ENC_Encode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value str(isolate, args[0]);
	int len = str.length();

	v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, len);
	memcpy(ab->GetContents().Data(), *str, len);
	
	args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, len));
}

// Uses V8's built-in, highly optimized UTF-8 decoder
void ENC_Decode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::Local<v8::Uint8Array> buffer = args[0].As<v8::Uint8Array>();
	
	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);

	v8::MaybeLocal<v8::String> str = v8::String::NewFromUtf8(isolate, data, v8::NewStringType::kNormal, len);
	
	if (str.IsEmpty()) {
		Throw(isolate, "Failed to decode UTF-8");
		return;
	}
	
	args.GetReturnValue().Set(str.ToLocalChecked());
}

void InitializeEncoding(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> enc_obj = v8::Object::New(isolate);
	SET_METHOD(enc_obj, "encode", ENC_Encode);
	SET_METHOD(enc_obj, "decode", ENC_Decode);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "encoding").ToLocalChecked(), enc_obj).Check();
}

