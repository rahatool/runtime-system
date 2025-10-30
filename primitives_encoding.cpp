#include "primitives.h"
#include <string>

void Encoding_Encode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value str(isolate, args[0]);
	size_t len = str.length();
	
	v8::Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, len);
	memcpy(ab->GetContents().Data(), *str, len);
	
	args.GetReturnValue().Set(v8::Uint8Array::New(ab, 0, len));
}

void Encoding_Decode(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	size_t len;
	char* data = GetUint8ArrayBufferData(args[0], &len);

	v8::MaybeLocal<v8::String> str = v8::String::NewFromUtf8(isolate, data, v8::NewStringType::kNormal, len);
	if (str.IsEmpty()) {
		Throw(isolate, "Failed to decode UTF-8 string");
		return;
	}
	args.GetReturnValue().Set(str.ToLocalChecked());
}

void InitializeEncoding(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> encoding_obj = v8::Object::New(isolate);
	SET_METHOD(encoding_obj, "encode", Encoding_Encode);
	SET_METHOD(encoding_obj, "decode", Encoding_Decode);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "encoding").ToLocalChecked(), encoding_obj).Check();
}

